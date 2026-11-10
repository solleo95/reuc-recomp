#include "clock.h"
#include "client_update_lock.h"
#include "cpu_topology.h"
#include "compile_screen.h"
#include "public_release.h"
#include "dispatch.h"
#include "dol.h"
#include "dvdfs.h"
#include "texture_replacement.h"
#include "fatal.h"
#include "crash_report.h"
#include "crash_bundle.h"
#include "gx_gap_audit.h"
#include "dispatch.h"
#include "game_decl.h"
#include "guest_mem.h"
#include "gx_audit.h"
#include "hle/os.h"
#include "hle/save.h"
#include "native_input.h"
#include "input_probe.h"
#include "entry_probe.h"
#include "game_input.h"
#include "input_config.h"
#include "options_config.h"
#include "host_input.h"
#include "gamepad_input.h"
#include "wiimote_input.h"
#include "coop_session.h"
#ifdef _WIN32
#include "raw_input_backend.h"
#else
#include "evdev_backend.h"
#endif
#include "rumble_hold.h"
#include <set>
#include "hle/gx.h"
#include "hle/os_layout.h"
#include "lowmem.h"
#include "mmio.h"
#include "sched.h"
#include "perf_counters.h"
#include "trace.h"
#include "vi.h"
#include "options.h"
#include "test_control.h"
#include "test_server.h"
#include "game_mods.h"
#include "mod_guest_call.h"
#include "menu_ui.h"
#include "test_state.h"
#include "test_draws.h"
#include "packet_dump.h"
#include "feature_report.h"
#include "upscaler_probe.h"
#include "intro.h"
#include "game_pacing.h"
#include "render_scale.h"
#include "window_mode.h"
#ifndef _WIN32
#include "window_icon.h"
#endif
#include "wide_view.h"
#include "late_aim.h"
#include "reticle_style.h"
#include "frame_limit.h"
#include "cpu_sampler.h"
#include "audio_output.h"
#include "ax_audio.h"
#include "det_trace.h"
#include "frame_log.h"
#include "hitch_report.h"
#include <array>
#include "diagnostic_log.h"
#include "startup_error.h"
#include "audio_gap_audit.h"
#include "game_renderer.h"
#include "host_keys.h"
#include "launch.h"
#include "live_settings.h"
#include "localization.h"
#include "prompt_hooks.h"
#include "settings_menu.h"
#include "deck_menu.h"
#include "ui_mode.h"
#include "steam_overlay.h"
#include <ctime>
#include "system_report.h"
#include "netplay_game.h"
#include "game_checkpoint.h"
#include "gx_scene.h"
#include <algorithm>
#include <future>
#include <atomic>
#include <chrono>
#include <deque>
#include <exception>
#include <functional>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <optional>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <timeapi.h>
#else
#include <execinfo.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#endif
#include "path_io.h"

extern const uc::FnEntry uc_functions[];
extern const size_t uc_function_count;
extern const uint32_t uc_text_begin, uc_text_end;
namespace fs = std::filesystem;

namespace {
uint32_t guest_writer() { // first translated function on the stack: the guest code storing to the FIFO
    void* frames[32];
#ifdef _WIN32
    const auto count = RtlCaptureStackBackTrace(1, 32, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) if (const auto guest = uc::guest_function_for_host(frames[i])) return guest;
#else
    const int count = backtrace(frames, 32);
    for (int i = 1; i < count; ++i) if (const auto guest = uc::guest_function_for_host(frames[i])) return guest;
#endif
    return 0;
}
struct FifoSink : uc::MmioDevice {   // native resource/vertex ingestion; unowned state bytes remain TD-014
    uint64_t bytes = 0;
    uint32_t read(uint32_t, int) override { return 0; }
    void write(uint32_t, int size, uint32_t value) override { bytes += uint64_t(size);
        if (uc::gx_audit_on.load(std::memory_order_relaxed)) uc::gx_audit_fifo_write(size,value);
        if (uc::gx_gap_audit_enabled()) uc::gx_gap_audit_write(size, value, uc::gx_fifo_in_primitive(), uc::gx_fifo_recording(), guest_writer);
        uc::gx_fifo_write(size, value); }
};
FifoSink g_fifo;
std::vector<std::unique_ptr<uc::RegFile>> g_regs;

void map_regs(uint32_t base, uint32_t size, const char* name, uint32_t mirror = 0) {   // mirror: second address window of the same registers
    g_regs.push_back(std::make_unique<uc::RegFile>(size));
    uc::mmio_bus().map(base, size, g_regs.back().get(), name);
    if (mirror) uc::mmio_bus().map(mirror, size, g_regs.back().get(), std::string(name) + " (mirror)");
}
void map_hardware() {   // register blocks of the Wii/GameCube as seen at 0xCC000000; plain read-back registers until a trace shows more is needed
    map_regs(0xCC000000, 0x80, "CP");     map_regs(0xCC001000, 0x100, "PE");  map_regs(0xCC002000, 0x100, "VI");
    map_regs(0xCC003000, 0x100, "PI");    map_regs(0xCC004000, 0x100, "MI");  map_regs(0xCC005000, 0x200, "DSP");
    map_regs(0xCC006000, 0x40, "DI", 0xCD006000);     map_regs(0xCC006400, 0x100, "SI", 0xCD006400);  map_regs(0xCC006800, 0x80, "EXI", 0xCD006800);
    map_regs(0xCC006C00, 0x20, "AI", 0xCD006C00);     map_regs(0xCD000000, 0x100, "IPC"); map_regs(0xCD800000, 0x1000, "HW");
    uc::mmio_bus().map(0xCC008000, 0x100, &g_fifo, "GX FIFO");
    uc::mmio_set_gx_fifo(&g_fifo);
}

std::atomic<uint64_t> g_fifo_snapshot{0};
gx::ReticleStyle reticle_style_of(const uc::ReticleOptions& options) {   // the menu's choices as the packet producer reads them (late_aim.h)
    gx::ReticleStyle style;
    style.hide_tail=options.hide_tail; style.hide_dot=options.hide_dot; style.hide_marks=options.hide_marks; style.hide_arc=options.hide_arc;
    style.color={options.color_p1,options.color_p2};
    style.dot_scale=options.dot_size/100.f; style.partner_alpha=options.partner_opacity/100.f;   // the wake's length goes to the renderer (set_wake_seconds)
    return style;
}

void print_stats() {
    auto vi = uc::vi_stats();
    auto d = uc::dvd_stats();
    std::fprintf(stderr, "stats: retraces %llu  dvd opens %llu  dvd reads %llu (%llu bytes)  fifo bytes %llu  unmapped mmio %zu\n",
                 (unsigned long long)vi.retraces, (unsigned long long)d.opens, (unsigned long long)d.reads, (unsigned long long)d.bytes,
                 (unsigned long long)g_fifo_snapshot.load(), uc::mmio_bus().unmapped_count());
    for (const auto& entry : uc::gx_audit_snapshot()) {
        std::fprintf(stderr, "gx audit: %08X calls %llu %s caller %08X  first r3..r10", entry.address, (unsigned long long)entry.calls, uc::symbol_name(entry.address).c_str(),entry.first_caller);
        for (auto arg : entry.first_gpr) std::fprintf(stderr, " %08X", arg);
        std::fprintf(stderr, "  f1..f6");
        for (auto arg : entry.first_fpr) std::fprintf(stderr, " %.6g", arg);
        std::fprintf(stderr, "\n");
        if (entry.address == 0x80288684) std::fprintf(stderr, "gx lists: nonempty %llu bytes %llu\n", (unsigned long long)entry.nonempty_lists, (unsigned long long)entry.list_bytes);
        if (entry.address == 0x80288528) {
            std::fprintf(stderr, "gx recordings: bytes %llu first caller %08X prefix", (unsigned long long)entry.recording_bytes,entry.first_caller);
            for (size_t i = 0; i < entry.first_recording_size; ++i) std::fprintf(stderr," %02X",entry.first_recording_prefix[i]);
            std::fprintf(stderr,"\n");
        }
            for (const auto& resource : entry.resources) {
                std::fprintf(stderr,"gx resource: address %08X size %zu captured %zu hex",resource.address,resource.size,resource.bytes.size());
                for (auto byte : resource.bytes) std::fprintf(stderr," %02X",std::to_integer<uint8_t>(byte));
                std::fprintf(stderr,"\n");
            }
    }
    auto gx = uc::gx_stats();
    std::fprintf(stderr, "gx stats: draws %llu vertices %llu submitted %llu completed %llu outside-primitive bytes %llu copies %llu\n",
        (unsigned long long)gx.draws, (unsigned long long)gx.vertices, (unsigned long long)gx.submitted, (unsigned long long)gx.completed, (unsigned long long)gx.outside_bytes, (unsigned long long)gx.copies);
}

fs::path exe_path() { return uc::exe_path(); }
fs::path exe_dir() { return exe_path().parent_path(); }
unsigned long process_id() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}
// The CPUs the process may run on as a bit mask (CPUs 0..63); the guest thread pinning frees the SMT siblings of its CPU through these.
bool get_affinity(uint64_t& mask) {
#ifdef _WIN32
    DWORD_PTR process = 0, system = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process, &system)) return false;
    mask = process;
    return true;
#elif defined(__APPLE__)
    (void)mask;
    return false;   // macOS has no CPU affinity: the system places threads by QoS (the guest threads ask for user-interactive, runtime/sched.cpp)
#else
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof set, &set)) return false;
    mask = 0;
    for (int cpu = 0; cpu < 64; ++cpu) if (CPU_ISSET(cpu, &set)) mask |= uint64_t(1) << cpu;
    return true;
#endif
}
bool set_affinity(uint64_t mask) {   // Linux: the calling thread and the threads it starts afterwards
#ifdef _WIN32
    return SetProcessAffinityMask(GetCurrentProcess(), mask) != 0;
#elif defined(__APPLE__)
    (void)mask;
    return false;
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int cpu = 0; cpu < 64; ++cpu) if (mask & (uint64_t(1) << cpu)) CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof set, &set) == 0;
#endif
}
const bool kPublicRelease = uc::public_release();   // an installed release client (release.json next to the exe) writes no logs, crash reports or debug files
std::vector<uint8_t> slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
struct VideoSession {
    VideoSession() { if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError()); }
    ~VideoSession() { SDL_Quit(); }
};
struct NativeInputSink : uc::TestInputSink {
    uc::NativeInput& input; uc::NativeInput* second; SDL_Window* window; const uint8_t* guest;const uc::NetGame* network;
    unsigned player=0;   // the player the test server is applying an input for (select_player)
    NativeInputSink(uc::NativeInput& i, uc::NativeInput* s, SDL_Window* w, const uint8_t* g,const uc::NetGame* n) : input(i), second(s), window(w), guest(g),network(n) {}
    void select_player(unsigned p) override { player=p; }
    std::string peek(uint32_t address, uint32_t size) override {
        // MEM1 24 MB at 0x80000000, MEM2 64 MB at 0x90000000 (guest_mem layout).
        const uint64_t end = uint64_t(address) + size;
        const bool mem1 = address >= 0x80000000u && end <= 0x80000000ull + (24u << 20), mem2 = address >= 0x90000000u && end <= 0x90000000ull + (64u << 20);
        if (!mem1 && !mem2) return {};
        const uint8_t* source = guest + (mem2 ? 0x10000000u + (address - 0x90000000u) : address - 0x80000000u);
        static const char digits[] = "0123456789ABCDEF"; std::string hex; hex.reserve(size_t(size) * 2);
        for (uint32_t i = 0; i < size; ++i) { hex += digits[source[i] >> 4]; hex += digits[source[i] & 15]; }
        return hex;
    }
    bool kpad(uint64_t sequence, float x, float y, uint32_t buttons) override { return input.apply_test_command(sequence, x, y, buttons); }
    bool host(uint64_t sequence, const uc::HostEvent& event) override {
        int width = 0, height = 0;
        if (!SDL_GetWindowSizeInPixels(window, &width, &height)) return false;
        if (player == 1) {   // the second player: the game sees a second pad from the first input for it on (as when a second player has joined)
            if (!second) return false;
            if (!second->apply_test_event(sequence, event, uint32_t(std::max(0, width)), uint32_t(std::max(0, height)))) return false;
            uc::input_hle_set_second_connected(true);
            return true;
        }
        return input.apply_test_event(sequence, event, uint32_t(std::max(0, width)), uint32_t(std::max(0, height)));
    }
    uint64_t consumed() override { return network ? network->consumed_sequence() : second ? std::max(input.last_consumed_sequence(), second->last_consumed_sequence()) : input.last_consumed_sequence(); }
};
struct GuestRunner {
    uc::Sched& sched;
    gx::Packets& packets;
    std::atomic<bool> done{false};
    std::thread coordinator;
    GuestRunner(uc::Sched& s, uc::Thread* main, gx::Packets& p) : sched(s), packets(p) {
        coordinator = std::thread([this, main] {
            try { sched.run(main); done.store(true); }
            catch (const std::exception& error) { uc::fatal("scheduler coordinator failed: %s", error.what()); }
        });
    }
    void stop() { sched.shutdown(); packets.cancel(); if (coordinator.joinable()) coordinator.join(); }
    ~GuestRunner() { stop(); }
};
}  // namespace

// Sleeps until `due` to within ~150 us: a high-resolution waitable timer for most of it, a spin for the rest (the reticle layer's refresh cadence).
#if defined(__APPLE__)
static void precise_sleep_until(std::chrono::steady_clock::time_point due) {   // no clock_nanosleep on macOS: a relative nanosleep to the margin, then the spin
    const auto margin=std::chrono::microseconds(150);
    const auto rest=due-margin-std::chrono::steady_clock::now();
    if(rest>std::chrono::nanoseconds(0)) {
        const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(rest).count();
        const timespec span{time_t(ns/1000000000),long(ns%1000000000)};
        nanosleep(&span,nullptr);
    }
    while(std::chrono::steady_clock::now()<due) _mm_pause();
}
#elif !defined(_WIN32)
static void precise_sleep_until(std::chrono::steady_clock::time_point due) {   // steady_clock is CLOCK_MONOTONIC with libstdc++
    const auto margin=std::chrono::microseconds(150);
    if(due-std::chrono::steady_clock::now()>margin) {
        const auto target=(due-margin).time_since_epoch();
        const timespec at{time_t(std::chrono::duration_cast<std::chrono::seconds>(target).count()),long(target.count()%1000000000)};
        clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&at,nullptr);
    }
    while(std::chrono::steady_clock::now()<due) _mm_pause();
}
#else
static void precise_sleep_until(std::chrono::steady_clock::time_point due) {
    static thread_local HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
    const auto margin=std::chrono::microseconds(150);
    const auto now=std::chrono::steady_clock::now();
    if(timer && due-now>margin) {
        LARGE_INTEGER relative; relative.QuadPart=-(std::chrono::duration_cast<std::chrono::nanoseconds>(due-margin-now).count()/100);
        if(SetWaitableTimer(timer,&relative,0,nullptr,nullptr,FALSE)) WaitForSingleObject(timer,INFINITE);
    }
    while(std::chrono::steady_clock::now()<due) YieldProcessor();
}
#endif
static int real_main(int argc, char** argv,uc::Options& startup_options) {
#ifdef _WIN32
    timeBeginPeriod(1);   // 1 ms timers: sleeps and the modal drag/resize loop's WM_TIMER tick (otherwise 15.6 ms granularity)
    {   // Windows 11 may run a window without focus in EcoQoS and ignore the timer request above; a game wants neither.
        PROCESS_POWER_THROTTLING_STATE throttling{PROCESS_POWER_THROTTLING_CURRENT_VERSION,PROCESS_POWER_THROTTLING_EXECUTION_SPEED|PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION,0};
        if(!SetProcessInformation(GetCurrentProcess(),ProcessPowerThrottling,&throttling,sizeof throttling)) std::fprintf(stderr,"power throttling opt-out failed: %lu\n",GetLastError());
    }
#endif
    std::vector<std::string_view> arguments;
    for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
    auto parsed = uc::parse_options(arguments);
    if (auto* error = std::get_if<std::string>(&parsed)) { std::fprintf(stderr, "%s\nusage: uc [--game <dir>] [--seconds N] [--headless] [--width N] [--height N] [--render-scale 0.5..6 (720p multiplier)] [--window-mode windowed|borderless|exclusive] [--no-ultrawide] [--hud-position auto|center|edges] [--pointer relative|absolute][--anisotropy 1|2|4|8|16] [--per-pixel-lighting] [--ambient-occlusion off|low|medium|high|ultra] [--motion-blur off|low|medium|high|ultra] [--contact-shadows] [--ray-tracing] [--rt-quality low|medium|high|ultra] [--dlss5] [--dlss5-option key=value] [--unlock-60fps] [--game-hz 61..480] [--validation] [--log-dvd] [--log-sleep] [--gx-audit] [--skip-intro] [--mute] [--no-mods] [--guest-cpu N] [--deterministic] [--det-out <file> [--det-frames N]] [--demo-record <file> | --demo-play <file>] [--stop-retrace N] [--ui desktop|deck] [--no-vulkan-layers] [--profile] [--input-probe] [--capture-scene-passes] [--test-server <name> [--speed 1..8] [--capture-dir <dir>] [--god] [--skip-tutorials]]\n", error->c_str()); return 2; }
    auto options = std::get<uc::Options>(std::move(parsed));
    startup_options=options;   // probe/harness failures remain noninteractive, including a bad config
    const auto config_path=options.config.empty() ? exe_dir()/"config.json" : options.config;
    const auto input_config=uc::load_config(config_path);
    parsed=uc::parse_options(arguments,uc::options_from_config(input_config));
    if(auto* error=std::get_if<std::string>(&parsed))throw std::invalid_argument(*error);
    options=std::get<uc::Options>(std::move(parsed));
    startup_options=options;
    uc::ConfigDocument diagnostic_options;uc::store_options(diagnostic_options,options);
    std::fprintf(stderr,"[settings] graphics=%s\n",diagnostic_options.properties["graphics"].dump().c_str());
    std::fprintf(stderr,"[settings] dlss5=%s\n",diagnostic_options.properties["dlss5"].dump().c_str());
    if(kPublicRelease) { options.gx_audit=false; options.capture_scene_passes=false; options.audio_audit=false; options.gx_gaps=false; }   // developer reports that write files
    { std::error_code ignored; if(!std::filesystem::exists(exe_dir()/uc::kDlss5Runtime,ignored)) options.dlss5_enabled=false; }   // the player supplies nvngx_dlssnr.dll; without it DLSS 5 stays off
    bool deck_view=false;   // Linux: the Steam Deck's own menu (Game Mode, or --ui deck); otherwise the menu of the Windows build
#ifndef _WIN32
    if(!options.features) {
        const auto ui_mode=uc::resolve_ui_mode(options.ui ? std::optional<std::string_view>(*options.ui) : std::nullopt,std::getenv("UC_UI"),std::getenv("GAMESCOPE_WAYLAND_DISPLAY"));
        if(const auto* error=std::get_if<std::string>(&ui_mode)) { std::fprintf(stderr,"%s\n",error->c_str()); return 2; }
        deck_view=std::get<uc::UiMode>(ui_mode)==uc::UiMode::deck;
        std::fprintf(stderr,"[ui] mode %s (%s)\n",std::string(uc::ui_mode_name(std::get<uc::UiMode>(ui_mode))).c_str(),options.ui ? "flag" : std::getenv("UC_UI") ? "env" : std::getenv("GAMESCOPE_WAYLAND_DISPLAY") ? "gamescope" : "default");
    }
#else
    if(options.ui) {   // Windows has one view: the flag is accepted (the launcher of another platform may pass it) but a wrong value is still an error
        const auto checked=uc::resolve_ui_mode(std::string_view(*options.ui),nullptr,nullptr);
        if(const auto* error=std::get_if<std::string>(&checked)) { std::fprintf(stderr,"%s\n",error->c_str()); return 2; }
    }
#endif
    if(options.features) {   // the launcher's support query: the same runtime files the game loads, no window
        uc::UpscalerSupport support;
        if(options.renderer_backend==uc::RendererBackend::vulkan) support=uc::probe_upscalers(exe_dir(),options.dlss5_enabled);
        else support=uc::probe_native_upscalers(exe_dir(),options.renderer_backend);
        uc::FeatureReport report{support.dlss,support.dlss_g,support.fsr,support.fsr_fg,support.dlss_g_max_multiplier,support.dlss_reason,support.dlss_g_reason,support.fsr_reason,support.fsr_fg_reason,support.effects,support.effects_reason,support.anisotropy_max,support.ray_tracing,support.ray_reconstruction,support.ray_tracing_reason,support.ray_reconstruction_reason,support.dlss5,support.dlss5_reason};
        std::printf("%s\n",uc::to_json(report).c_str());
        return 0;   // the caller reads the report as soon as it is written and does not wait for this process to unload the NGX/Streamline runtimes
    }
    if(!uc::diagnostic_directory().empty())uc::log_system_report(exe_dir());   // only while a log is written
    const bool server_mode=!options.test_server.empty();
    if(server_mode && !options.test_control.empty()) throw std::invalid_argument("--test-server replaces --test-control; use only one");
    const bool controlled=!options.test_control.empty() || server_mode;
    if(controlled && (options.headless || options.save_root.empty())) throw std::invalid_argument("--test-control and --test-server require a renderer and an isolated --save-root");
    if(controlled && options.window_mode!=uc::WindowMode::windowed) throw std::invalid_argument("process-local diagnostics require Windowed mode to preserve desktop focus");
    const auto& game = options.game;
    bool fixed_hdr_levels=false;   // Game Mode: peak and paper white are the Deck's, not the config's (the menu sends its copy of them with every HDR change)
#ifdef _WIN32
    if(options.no_vulkan_layers && !SetEnvironmentVariableA("VK_LOADER_LAYERS_DISABLE","~implicit~")) throw std::runtime_error("cannot disable implicit Vulkan layers");   // read by the loader at vkCreateInstance
#else
    if(options.no_vulkan_layers && setenv("VK_LOADER_LAYERS_DISABLE","~implicit~",1)) throw std::runtime_error("cannot disable implicit Vulkan layers");
    // Game Mode: gamescope's own Vulkan layer is what offers an HDR10 swapchain (Proton sets the same variables for HDR games); it stays on when the other implicit layers are off.
    // Game Mode: HDR is on by default (the checkbox turns it off) and its peak and paper white are fixed here for the Deck OLED, whatever config.json holds. The panel holds 600 nits
    // over the whole screen but 1000 only in small areas (its limiter dims a large bright area): 300 nits paper white keeps the mid-tones as bright as SDR at a normal brightness, and a 400 nits
    // peak keeps white areas and the bloom's glow (which the HDR encode adds up to the peak) from blowing out; 600 did.
    if(std::getenv("GAMESCOPE_WAYLAND_DISPLAY")) {
        setenv("ENABLE_GAMESCOPE_WSI","1",1); setenv("ENABLE_HDR_WSI","1",1); setenv("VK_LOADER_LAYERS_ENABLE","VK_LAYER_FROG_gamescope_wsi*",1);
        options.hdr.peak_nits=400; options.hdr.paper_white_nits=300;   // the interface brightness (ui_nits) stays the player's: a slider
        fixed_hdr_levels=true;
    }
#endif
    uc::intro_init(options.skip_intro);
    // Network play steps at the session tick whatever the frame limit says; the limit then only caps presents.
    const auto pacing=options.netplay ? uc::PacingMode{options.unlock_60fps,options.game_hz} : uc::pacing_for_limit(options.fps_limit,options.game_hz,options.unlock_60fps,server_mode && !options.deterministic);   // recorded demos retain their virtual step
    uc::game_pacing_init(pacing.unlock_60,pacing.game_hz);
    if (options.gx_audit) {
#ifdef UC_TRACE
        uc::gx_audit_enable();
#else
        std::fprintf(stderr, "--gx-audit requires a trace build (scripts\\bringup.bat)\n"); return 2;
#endif
    }
    uc::os_log_sleep(options.log_sleep);
    uc::gx_gap_audit_enable(options.gx_gaps);
    uc::install_crash_handlers();
    uc::set_exit_hook(print_stats);
    uc::crash_report_init(kPublicRelease ? fs::path() : (uc::diagnostic_directory().empty() ? exe_dir()/"logs" : uc::diagnostic_directory())/"crashes", [] {   // an empty directory writes no report
        const auto vi = uc::vi_stats(); const auto d = uc::dvd_stats(); const auto gx = uc::gx_stats(); const auto sound = uc::audio_stats();
        char text[512];
        std::snprintf(text, sizeof text, "retraces %llu\ndvd opens %llu reads %llu\ngx draws %llu submitted %llu completed %llu copies %llu unowned fifo bytes %llu\naudio frames %llu nonzero samples %llu\n",
            (unsigned long long)vi.retraces, (unsigned long long)d.opens, (unsigned long long)d.reads, (unsigned long long)gx.draws,
            (unsigned long long)gx.submitted, (unsigned long long)gx.completed, (unsigned long long)gx.copies, (unsigned long long)gx.outside_bytes,
            (unsigned long long)sound.frames, (unsigned long long)sound.nonzero_samples);
        std::string state(text);
        try {   // guest registers of the crashed thread and the memory behind r3/r4 (the usual object/argument pointers)
            if (const auto* thread = uc::os_sched().current()) {
                char line[96];
                state += "guest registers of the current thread:";
                for (unsigned i = 0; i < 32; ++i) { std::snprintf(line, sizeof line, "%s r%u=%08X", i % 8 == 0 ? "\n" : "", i, thread->ctx.r[i]); state += line; }
                std::snprintf(line, sizeof line, "\nlr=%08X\n", thread->ctx.lr); state += line;
                for (const uint32_t reg : {3u, 4u}) {
                    const uint32_t address = thread->ctx.r[reg];
                    if (address < 0x80000000u || address > 0x817FFFC0u) continue;
                    std::snprintf(line, sizeof line, "memory at r%u (%08X):", reg, address); state += line;
                    for (unsigned i = 0; i < 64; ++i) { std::snprintf(line, sizeof line, "%s%02X", i % 16 == 0 ? "\n  " : " ", thread->ctx.mem[(address & 0x3FFFFFFFu) + i]); state += line; }
                    state += "\n";
                }
            }
        } catch (...) {}
        return state;
    });
    uc::crash_report_set_bundle([](const fs::path& dir) {   // guest memory, thread registers, GX arrays and function table: `python -m uctest inspect <report>` reads them back
        std::string threads;
        try { threads = uc::os_sched().dump_contexts(); } catch (...) { threads = "unavailable\n"; }   // only the baton holder may read it safely
        uc::write_crash_bundle(dir, threads, uc::gx_state_summary());
    });
    for (const char* n : {"identified_sdk.txt", "symbols.txt", "gx_symbols.txt"}) {   // installed next to the exe, or the project folder of a build tree
        for (const fs::path& d : {exe_dir() / "data", exe_dir() / ".." / ".." / "project"}) if (fs::exists(d / n)) { uc::symbols_load((d / n).string()); break; }
    }

    auto dol_bytes = slurp(game / "DATA" / "sys" / "main.dol");
    auto dol = uc::parse_dol(dol_bytes);
    if (!dol.ok) uc::fatal("cannot load %s: %s", (game / "DATA" / "sys" / "main.dol").string().c_str(), dol.error.c_str());

    uc::guest_mem_init();
    ppc::Ctx boot{};
    boot.mem = uc::guest_mem_base();
    for (const auto& s : dol.image.sections) std::memcpy(boot.mem + (s.address & 0x3FFFFFFFu), dol_bytes.data() + s.file_offset, s.size);   // BSS is already zero
    uint8_t hdr[32] = {};
    auto boot_bin = slurp(game / "DATA" / "sys" / "boot.bin");
    std::memcpy(hdr, boot_bin.data(), std::min<size_t>(32, boot_bin.size()));
    uc::LowMemInfo info;
    info.arena_lo = uc::arena_lo_after_bss(dol.image.bss_address, dol.image.bss_size);
    uc::init_lowmem(boot, hdr, info);

    map_hardware();
    uc::dispatch_init(uc_functions, uc_function_count, uc_text_begin, uc_text_end);
    static const uc::mods::GuestCallBackend guest_calls{&uc::has_function, &uc::call_indirect};   // mods calling game functions (modding/mod_guest_call.h)
    uc::mods::set_guest_call_backend(&guest_calls);
    uc::DvdFs dvd;
    if (!dvd.open_root(game / "DATA" / "files")) uc::fatal("game files not found under %s", (game / "DATA" / "files").string().c_str());
    // Mods: their file overlays go into the disc tree now, before the game opens anything; the scripts start once the guest is about to run.
    uc::GameMods mods(exe_dir(), input_config.properties.contains("mods") ? input_config.properties["mods"] : nlohmann::ordered_json::object(), dvd,
                      std::string(reinterpret_cast<const char*>(hdr), 6), options.no_mods);
    uc::Sched sched(boot.mem);
    std::unique_ptr<uc::DetTrace> det_trace;
    const bool deterministic = options.deterministic || !options.det_out.empty();
    if (deterministic) {
        uc::set_deterministic_clock(true);
        sched.set_deterministic(true);
        // Network play is deterministic too, but frame generation only adds presented frames: the host shows more than the shared tick.
        if (options.frame_generation && !options.netplay) { options.frame_generation = false; std::fprintf(stderr, "Frame generation: off in deterministic runs\n"); }
    }
    if (!options.det_out.empty()) det_trace = std::make_unique<uc::DetTrace>(options.det_out, boot.mem, options.det_frames);
    std::atomic<bool> det_stop{false};
#ifndef _WIN32
    if (options.guest_cpu < 0 && !std::getenv("UC_NO_GUEST_PIN")) {
        // Linux (the Steam Deck): the guest thread runs on one physical core whose SMT sibling stays free of every other thread of the process (measured on the Deck: +5% FPS in the
        // first scene over the system's scheduling). The second core: the first one takes the system's interrupts.
        uint64_t allowed = 0;
        try {
            const auto cores = uc::cpu_topology().cores;
            if (get_affinity(allowed) && cores.size() >= 4 && cores[1].mask != (uint64_t(1) << cores[1].cpu) && (allowed & cores[1].mask) == cores[1].mask) options.guest_cpu = cores[1].cpu;
        } catch (const std::exception& error) { std::fprintf(stderr, "cannot inspect the CPU topology: %s\n", error.what()); }
    }
#endif
    if (options.guest_cpu >= 0) {
        uint64_t process_mask = 0;
        if (get_affinity(process_mask)) {
            const uint64_t own = uint64_t(1) << options.guest_cpu;
            if(process_mask & own) {
                try {
                    const auto mask=uc::cpu_worker_mask(uc::cpu_topology().cores,options.guest_cpu,process_mask);
                    if(mask!=process_mask && !set_affinity(mask))std::fprintf(stderr,"cannot free the SMT siblings of CPU %d\n",options.guest_cpu);
                } catch(const std::exception& error) {std::fprintf(stderr,"cannot inspect SMT siblings: %s\n",error.what());}
                sched.pin_guest_threads(options.guest_cpu);
            } else std::fprintf(stderr,"guest CPU %d unavailable; using the system's scheduling\n",options.guest_cpu);
        }
    }
    uc::NativeInput input;
    uc::NativeInput input2;   // the second player's input (local two-player play: CoopSession decides which devices feed it)
    std::unique_ptr<uc::NetGame> network;
    const auto local_save_root=options.save_root.empty() ? exe_dir()/"saves" : options.save_root;
    if(options.netplay)network=std::make_unique<uc::NetGame>(options,input,exe_path(),local_save_root);
    uc::SaveFs saves(network ? network->save_root() : local_save_root);
    uc::save_hle_init(saves);
    uc::DvdSession dvd_session(dvd);
    uc::dvd_log(options.log_dvd,!uc::diagnostic_directory().empty());
    uc::set_clock_speed(options.speed);
    uc::test_state_enable(server_mode);
    uc::test_state_set_god(options.god);
    uc::test_state_set_skip_tutorials(options.skip_tutorials);
    uc::os_hle_init(sched, boot.mem, 0x817F0000);
    sched.irq_ctx().r[1] = 0x817EFF00;
    uc::vi_hle_init(sched, uc::kViPeriodNtsc, [&] {
        g_fifo_snapshot.store(g_fifo.bytes, std::memory_order_relaxed);
        const uint64_t retraces = uc::vi_stats().retraces;
        if (det_trace) det_trace->on_retrace(retraces);
        if (deterministic && retraces % 60 == 0) {   // identical work per game second: the wall time and guest cycles between marks are what builds are compared on
            static auto last = std::chrono::steady_clock::now(); static uint64_t last_draws = 0, last_cycles = 0;
            const auto now = std::chrono::steady_clock::now(); const uint64_t draws = uc::gx_stats().draws, cycles = sched.guest_cycles();
            std::fprintf(stderr, "[detbench] retrace %llu draws %llu guest_mcycles %.2f wall_ms %.2f", (unsigned long long)retraces, (unsigned long long)(draws - last_draws), (cycles - last_cycles) / 1e6, std::chrono::duration<double, std::milli>(now - last).count());
            if (const uc::PerfTotals hw = uc::perf_totals(); hw.enabled) {   // UC_PERF_COUNTERS=1: user-space instructions and cycles of the guest threads since the previous mark
                static uint64_t last_instructions = 0, last_hw_cycles = 0;
                std::fprintf(stderr, " guest_minstr %.4f guest_hw_mcycles %.4f", (hw.instructions - last_instructions) / 1e6, (hw.cycles - last_hw_cycles) / 1e6);
                last_instructions = hw.instructions; last_hw_cycles = hw.cycles;
            }
            {   // the wall time the baton was held since the previous mark: the serial time of the guest, which work moved to helper threads shortens (hw counters do not see it)
                static uint64_t last_serial = 0; uint64_t serial = 0;
                for (int id = 1; id < 32; ++id) serial += sched.thread_run_ns(id);
                std::fprintf(stderr, " guest_serial_ms %.3f", (serial - last_serial) / 1e6);
                last_serial = serial;
            }
            std::fprintf(stderr, "\n");
            last = now; last_draws = draws; last_cycles = cycles;
        }
        if (options.stop_retrace && retraces >= options.stop_retrace) det_stop = true;
        static const char* sample_flag = std::getenv("UC_CPU_SAMPLE_FLAG"); static const char* sample_from = std::getenv("UC_CPU_SAMPLE_RETRACE");
        if (sample_flag && sample_from && retraces == std::strtoull(sample_from, nullptr, 10)) { if (std::FILE* f = std::fopen(sample_flag, "w")) std::fclose(f); }   // starts the CPU sampler (cpu_sampler.cpp) at this retrace
    });

    // The start code runs on the SDK's DefaultThread: __OSThreadInit (802611B8) stores this OSThread in the current-thread word and fills it in.
    const uint32_t kMainThread = 0x805F88D8;
    uint32_t entry = dol.image.entry;
    uc::Thread* mt = sched.create(kMainThread, 16, [entry,&network](ppc::Ctx& c) {
        if(network)network->prepare();
        c.r[1] = 0x817D0000;   // the start code installs its own stack pointer; this one only has to be valid
        uc::call_indirect(c, entry);
    }, &boot);

    std::optional<VideoSession> video;
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(nullptr, SDL_DestroyWindow);
    std::optional<uc::GameRenderer> renderer_storage; renderer_storage.emplace(options.renderer_backend);
    uc::GameRenderer& renderer=*renderer_storage;   // restart_renderer() replaces it in the same storage, so this reference stays valid
    double render_aspect=16.0/9;
    // Native ultrawide (renderer/ultrawide.h): the packets are scaled for the shape of the output, whatever shape the EFB was created in.
    int view_width=0,view_height=0;   // the last output size the factors were made for (a change of the interface position remakes them)
    const auto view_for_output=[&options,&view_width,&view_height,&network,deterministic](int width,int height) {
        if(width<=0 || height<=0) return;
        view_width=width; view_height=height;
        const double aspect=double(width)/height;
        const bool edges=options.hud_position==uc::HudPosition::edges || (options.hud_position==uc::HudPosition::automatic && aspect<=2.45);   // automatic: up to about 21:9
        // The wide picture changes what the game computes (its projection, the pointer's range, hit tests): never in a run that must be the same on every machine
        // (network play, deterministic runs, demos), where the game keeps its 16:9 and the picture is stretched as before.
        const bool shared_state=network || deterministic || !options.demo_record.empty() || !options.demo_play.empty();
        gx::set_view_factors(options.ultrawide && !shared_state ? gx::factors_for_aspect(aspect,edges) : gx::ViewFactors{});
    };
    const auto menu_language=[&]{ const auto configured=uc::launcher::config_language(input_config); return configured=="system" ? uc::launcher::system_language() : configured; }();   // the launcher's language: menus and start screens
    // Creates the renderer's device and pipelines for the current options (startup and every upscaler / frame generation switch).
    const auto init_renderer=[&](bool compile_screen) {
        const auto renderer_start=std::chrono::steady_clock::now();
        std::fprintf(stderr,"[startup] renderer=%s initialization begin scale=%.3f upscaler=%s RT=%d HDR=%d\n",std::string(uc::renderer_backend_name(options.renderer_backend)).c_str(),options.render_scale,std::string(uc::upscaler_name(options.upscaler)).c_str(),int(options.effects.ray_tracing),int(options.hdr.enabled));
        std::optional<uc::CompileScreen> screen;
        if(compile_screen) { screen.emplace(menu_language); if(!controlled) screen->show(); }   // test runs keep it hidden until a slow compile
        renderer.set_aim_source([&input] { return gx::clamp_aim(input.latest_aim()); });   // the reticle stays on the screen as the game's own does; the pointer runs on
        renderer.set_aim_history([&input](double seconds) { return gx::clamp_aim(input.aim_ago(seconds)); });
        renderer.configure_upscaling(options.upscaler, options.upscale_mode, options.sharpening, options.sharpness, options.frame_generation, options.frame_generation_multiplier);
        renderer.set_hdr(options.hdr);   // before the first swapchain: it picks HDR10 or SDR
        renderer.allow_dlss5(options.dlss5_enabled);
        try { renderer.initialize(window.get(), options.validation, exe_dir() / "data" / "shaders", options.render_scale,render_aspect,options.window_mode==uc::WindowMode::exclusive); }
        catch(const std::exception& error) { std::fprintf(stderr,"[startup] renderer initialization failed: %s\n",error.what()); throw; }   // said before the unwinding, whose own failures can end the process
        std::fprintf(stderr,"[startup] renderer initialized after %.1f ms; setting effects\n",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-renderer_start).count());
        renderer.set_prompt_style(options.prompt_style);
        renderer.set_bloom(options.bloom_settings());
        renderer.set_crosshair_scale(options.crosshair_scale);
        gx::set_reticle_style(reticle_style_of(options.reticle));
        renderer.set_effects(options.effects);
        renderer.set_dlss5(options.dlss5_enabled ? options.dlss5 : uc::Dlss5Settings{});
        if(compile_screen) {   // compile every known pipeline now, so no pipeline is built while the game runs
            auto next_report=std::chrono::steady_clock::time_point{};
            renderer.warm_pipelines([&](size_t done,size_t total) {
                screen->update(done,total);
                const auto now=std::chrono::steady_clock::now();
                if(now>=next_report || done==total) {
                    std::fprintf(stderr,"[startup] shaders %zu/%zu elapsed_ms=%.1f\n",done,total,std::chrono::duration<double,std::milli>(now-renderer_start).count());
                    next_report=now+std::chrono::seconds(1);
                }
            });
        } else renderer.warm_pipelines([](size_t,size_t) {});   // a restart: the pipelines come from the disk cache
        std::fprintf(stderr,"[startup] graphics ready after %.1f ms\n",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-renderer_start).count());
    };
    if (!options.demo_record.empty()) input.demo_record(options.demo_record);
    if (!options.demo_play.empty()) input.demo_play(options.demo_play);
    input.bindings(input_config.input);
    input.pad_settings(options.gamepad);
    input.wiimote_settings(options.wiimote);
    input2.bindings(options.player2.bindings);   // the second player: own mouse and gamepad tables, the first player's Wii Remote table
    input2.pad_settings(options.player2.gamepad);
    input2.wiimote_settings(options.wiimote);
    input2.pointer_smoothing(options.pointer_smoothing/100.f);
    input2.reload_zone(options.reload_zone,options.reload_zone_size/100.f);
    uc::HostInput host_input;
    host_input.sensitivity(options.mouse_sensitivity);
    host_input.pointer(options.pointer);
#ifndef _WIN32
    host_input.touch_pointer(true);   // Linux is the Steam Deck: its touchscreen is the pointer
#endif
    input.pointer_smoothing(options.pointer_smoothing/100.f);
    input.reload_zone(options.reload_zone,options.reload_zone_size/100.f);
    // Who owns the mouse while the game plays. relative: SDL's relative mode (hidden cursor, raw motion). absolute (lightguns, which move the Windows cursor): the cursor is
    // hidden and locked to the window and its window position is the pointer. Not playing (menu): the cursor is free in both.
    const auto capture_pointer=[&](bool playing) {
        if(controlled) return;
        const bool absolute=options.pointer==uc::PointerMode::absolute;
        if(!SDL_SetWindowRelativeMouseMode(window.get(),playing && !absolute)) throw std::runtime_error(SDL_GetError());
        if(!SDL_SetWindowMouseGrab(window.get(),playing && absolute)) throw std::runtime_error(SDL_GetError());
        if(playing && absolute) SDL_HideCursor(); else SDL_ShowCursor();
    };
    const auto pointer_captured=[&] { return options.pointer==uc::PointerMode::absolute ? SDL_GetWindowMouseGrab(window.get()) : SDL_GetWindowRelativeMouseMode(window.get()); };
    std::optional<uc::GamepadInput> gamepad;   // real gamepads feed the game window only: not in headless or harness runs (those inject pad events through the test server)
    if(!options.headless && (!controlled || std::getenv("UC_TEST_REAL_PAD"))) {   // UC_TEST_REAL_PAD: a harness run that pays the real pad's cost too (its events are still ignored there)
        try { gamepad.emplace(); }
        catch(const std::exception& error) { std::fprintf(stderr,"[gamepad] gamepads unavailable, keyboard and mouse only: %s\n",error.what()); }   // a broken joystick backend must not stop the game
    }
    // Local two-player play: only in the desktop menu's game window (not the Steam Deck's), never in network play, demos, deterministic runs and harness runs.
    const bool coop_enabled=!options.headless && !controlled && !network && !deck_view && !deterministic && options.demo_record.empty() && options.demo_play.empty();
    std::set<std::string> wii_claims;   // the remotes the readers have: each reader opens a remote the other one has not
    const auto wii_opener=[&wii_claims](std::string& error) { return uc::wii_open_unclaimed(wii_claims,error); };
    std::optional<uc::WiimoteInput> wiimote,wiimote2;   // a real Wii Remote feeds the game window only, like the gamepads: the harness injects wii_* events through the test server; the second reader looks for another remote (the second player's)
    if(!options.headless && !controlled && !deck_view) wiimote.emplace(wii_opener);   // the Steam Deck's menu has no Wii Remote support: nothing looks for one there
    if(coop_enabled) wiimote2.emplace(wii_opener);
    if (!options.headless) {
        if (options.width > INT32_MAX || options.height > INT32_MAX) throw std::invalid_argument("window extent exceeds integer limits");
        SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE,"0",SDL_HINT_OVERRIDE);
        SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_SPEED_SCALE,"1",SDL_HINT_OVERRIDE);
        video.emplace();
        const auto display=SDL_GetPrimaryDisplay();
        const auto* mode=display ? SDL_GetDesktopDisplayMode(display) : nullptr;
        if(!mode || mode->w<=0 || mode->h<=0) throw std::runtime_error("monitor display mode unavailable: "+std::string(SDL_GetError()));
        const double aspect=options.width && options.height ? double(options.width)/options.height : double(mode->w)/mode->h; render_aspect=aspect;   // an explicit window size sets the picture's shape, otherwise the monitor does
#ifdef __ANDROID__
        options.window_mode=uc::WindowMode::borderless;   // an app on a phone or handheld fills the screen: SDL hides the system bars
#endif
        const bool monitor_window=!options.width && !options.height;
        if(!options.width) options.width=uint32_t(mode->w);
        if(!options.height) options.height=uint32_t(mode->h);
#ifdef __ANDROID__   // 1x (720p) by default on a handheld: with Qualcomm's driver every draw takes the ubershader, GPU bound at 1.5x (20 fps on the Thor's title, 90 at 1x)
        if(!options.render_scale) options.render_scale=1.0;
#endif
        if(!options.render_scale) options.render_scale=uc::monitor_render_scale(uint32_t(mode->w),uint32_t(mode->h));
        std::fprintf(stderr,"Monitor: %dx%d %.3g Hz;720p multiplier %.4gx\n",mode->w,mode->h,mode->refresh_rate,options.render_scale);
        int window_width=int(options.width),window_height=int(options.height);
        if(monitor_window && options.window_mode==uc::WindowMode::windowed) {   // windowed start: the selected render resolution (shrunk only to fit the desktop)
            SDL_Rect usable{0,0,mode->w,mode->h};
            SDL_GetDisplayUsableBounds(display,&usable);
            const auto extent=uc::windowed_extent(options.render_scale,aspect,uint32_t(std::max(0,usable.w)),uint32_t(std::max(0,usable.h)));
            window_width=int(extent.width); window_height=int(extent.height);
        }
        const SDL_WindowFlags graphics_window=options.renderer_backend==uc::RendererBackend::vulkan ? SDL_WINDOW_VULKAN : 0;
        window.reset(SDL_CreateWindow("Resident Evil: The Umbrella Chronicles", window_width, window_height, graphics_window | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN));
        if (!window) throw std::runtime_error(SDL_GetError());
#ifndef _WIN32
        uc::set_window_icon(window.get());   // Windows has the icon in the exe (app.rc)
#endif
        // Enter fullscreen only after the compile overlay closes and Vulkan is ready to present.
        init_renderer(true);
#ifdef _WIN32
        if(controlled) {
            const auto properties=SDL_GetWindowProperties(window.get());
            const auto hwnd=static_cast<HWND>(SDL_GetPointerProperty(properties,SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr));
            if(!hwnd) throw std::runtime_error("diagnostic window handle unavailable");
            // A test window must never take the focus (an unfocused window runs slower than a focused one, which would skew measurements), not even when clicked.
            // WS_EX_NOACTIVATE alone hides the taskbar button; WS_EX_APPWINDOW keeps it.
            SetWindowLongPtrW(hwnd,GWL_EXSTYLE,GetWindowLongPtrW(hwnd,GWL_EXSTYLE)|WS_EX_NOACTIVATE|WS_EX_APPWINDOW);
            ShowWindow(hwnd,SW_SHOWNOACTIVATE);
            if(!SetWindowPos(hwnd,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE))
                throw std::runtime_error("cannot raise diagnostic window: "+std::to_string(GetLastError()));
        } else
#endif
        if(!SDL_ShowWindow(window.get())) throw std::runtime_error(SDL_GetError());   // Linux: a test window is shown like any other (the compositor decides about focus)
        uc::configure_window_mode(window.get(),options.window_mode,int(options.width),int(options.height));
        int output_width=0,output_height=0;
        if(!SDL_GetWindowSizeInPixels(window.get(),&output_width,&output_height))throw std::runtime_error(SDL_GetError());
        renderer.resize(uint32_t(std::max(0,output_width)),uint32_t(std::max(0,output_height)));
        view_for_output(output_width,output_height);
        const char* mode_name=options.window_mode==uc::WindowMode::windowed ? "Windowed" : options.window_mode==uc::WindowMode::borderless ? "Borderless" : "Exclusive Fullscreen";
        std::fprintf(stderr,"Window mode: %s\n[startup] window shown after shader warmup; output=%dx%d focused=%d\n",mode_name,output_width,output_height,int((SDL_GetWindowFlags(window.get())&SDL_WINDOW_INPUT_FOCUS)!=0));
        capture_pointer(true);
        input.focus(controlled || (SDL_GetWindowFlags(window.get())&SDL_WINDOW_INPUT_FOCUS)!=0);
    }
    // The second input exists for local two-player play and for harness runs (which send a second player's inputs with "player":1); the game sees a second pad once a second player has joined.
    const bool second_available=coop_enabled || (controlled && !network && !options.headless);
    uc::input_hle_init(options.headless ? nullptr : &input,second_available ? &input2 : nullptr);
    uc::game_input_init(options.headless ? nullptr : &input,options.input_probe,second_available ? &input2 : nullptr);
    if(second_available) { uc::input_hle_set_second_connected(false); input2.focus(input.focused()); }
    std::unique_ptr<uc::PointerBackend> pointer_backend;   // mice and light guns as devices of their own: who pressed a button decides who joins
    if(coop_enabled) {
#ifdef _WIN32
        pointer_backend=uc::make_raw_input_backend(window.get());
#elif !defined(__APPLE__)   // macOS: none yet, both players' mice are SDL's one mouse (gamepads still tell the players apart)
        pointer_backend=uc::make_evdev_pointer_backend();
#endif
    }
    uc::CoopSession coop({input,input2,gamepad ? &*gamepad : nullptr,{wiimote ? &*wiimote : nullptr,wiimote2 ? &*wiimote2 : nullptr},pointer_backend.get()},coop_enabled);
    coop.sensitivity(options.mouse_sensitivity);
    if(options.netplay) {
        network->install(sched,saves);
    }
    if(const char* probe=std::getenv("UC_ROLLBACK_PROBE_RETRACE")) {
        if(!deterministic || options.demo_play.empty() || options.netplay)
            throw std::invalid_argument("rollback probe requires an offline deterministic input demo");
        size_t parsed=0;
        const auto retrace=std::stoull(probe,&parsed);
        if(probe[parsed]!='\0') throw std::invalid_argument("invalid rollback probe retrace");
        saves.begin_rollback();
        uc::install_rollback_probe(sched,saves,input,retrace,8);
    }
    uc::input_probe_init(options.input_probe);
    uc::entry_probe_init(options.input_probe);
    uc::prompt_hooks_init(input_config.input,options.input_probe);
#ifdef _WIN32
    bool pad_last_used=false,shown_pad_last=false;
#else
    bool pad_last_used=true,shown_pad_last=true;   // Linux is the Steam Deck: gamepad prompts from the first frame
#endif
   // the last device used was a gamepad (control type "auto" shows that device's prompts); what the prompts were built for
    unsigned pad_changes=0;
    bool wii_last_used=false,shown_wii_last=false;   // the last device used was the Wii Remote (control type "auto" shows its prompts, in its look)
    unsigned wii_changes=0,wii_activity=0,wii_activity2=0;
    const auto refresh_prompt_device=[&] {   // the gamepad's (or the Wii Remote's) prompts: its table and the look of the connected pad or the chosen model
        const auto status=gamepad ? gamepad->status() : uc::PadStatus{};
        const auto& pad=options.gamepad;
        if(wii_last_used && pad.type==uc::ControlType::automatic && options.wiimote.enabled && ((wiimote && wiimote->status().connected) || (wiimote2 && wiimote2->status().connected))) {
            uc::prompt_hooks_set_gamepad(options.wiimote.bindings,uc::PromptDevice{false,uc::PadFamily::generic,false,true});
            return;
        }
        uc::prompt_hooks_set_gamepad(pad.bindings,uc::resolve_prompt_device(pad.type,pad.model,pad_last_used,status.connected,status.family,pad.gyro.mode!=uc::GyroMode::off,status.has_gyro));
    };
    refresh_prompt_device();
    uc::input_hle_set_probe(options.input_probe ? uc::input_probe_sample : nullptr);
    std::optional<uc::TestPipes> test_pipes;
    std::optional<NativeInputSink> test_sink;
    std::optional<uc::TestServer> test_server;
    if (server_mode) { test_pipes.emplace(options.test_server); test_sink.emplace(input, second_available ? &input2 : nullptr, window.get(), boot.mem,network.get()); test_server.emplace(*test_pipes, *test_sink); }
    bool test_released = false, draws_wanted = false; uint32_t draws_pass = 0; std::vector<uc::TestDrawStat> draw_stats; std::string draw_projection;
    double last_render_ms = 0;
    std::optional<uc::AudioOutput> audio;
    if (!options.headless) audio.emplace(options.mute);
    if (audio && options.netplay) audio->mute_on_gaps(true);
    uc::audio_gap_audit_enable(options.audio_audit);
    uc::audio_hle_init(sched,[&audio](std::span<const int16_t> pcm) { if (audio) audio->write(pcm); },options.profile);
    gx::Packets packets{bool(network)};
    uc::gx_hle_init(sched, boot.mem, options.headless ? nullptr : &packets, options.profile);
    if (!options.headless) {
        uc::gx_enable_async_textures();
        // Replacement textures (BC7 .dds named like the dumped PNGs) sit next to the exe; nothing happens without the folder.
        if (const auto count = gx::texture_replacements().load_index(exe_dir() / "textures")) {
            std::fprintf(stderr, "texture replacements: %zu files\n", count);
            uc::gx_set_texture_budget(size_t(2) << 30);
        }
        if (const auto count = mods.load_textures()) {   // later mods win over earlier ones and over the base folder
            std::fprintf(stderr, "texture replacements with mods: %zu files\n", count);
            uc::gx_set_texture_budget(size_t(2) << 30);
        }
    }
    sched.set_shutdown_hook([&packets,&network] { if(network)network->cancel(); packets.cancel(); });
    uc::mods::ModMenuUi mod_menu([&mods] { return mods.registry(); },[&mods] { return mods.host(); },menu_language);   // the Mods tab and the mods' own tabs and widgets
    mods.set_texture_handler([] { uc::gx_set_texture_budget(size_t(2) << 30); });   // textures of a mod switched on live need the larger cache too
    mod_menu.set_status_line([&mods](const std::string& id) { return mods.mod_status(id); });   // audio conversion progress and the audio and text problems under the mod
    mod_menu.set_apply([&mods] { mods.apply(); });   // the manager's changes (on, off, order, native code) reach the running game
    mods.start(boot.mem,options.headless ? nullptr : &mod_menu);   // on_load runs here; the guest starts right below
    GuestRunner runner(sched, mt, packets);
    std::optional<uc::CompileScreen> network_wait_screen;
    auto start = std::chrono::steady_clock::now(), next_watch = start + std::chrono::seconds(1);
    auto last_profile_time=start;
    uint64_t last_profile_retrace=0;
    uint64_t last_retraces = 0;
    uint64_t last_profile_presents=0,last_profile_textures=0,last_profile_decodes=0,last_profile_evictions=0,render_ns=0;
    uint64_t loop_wait_ns=0,profile_loops=0,profile_retries=0,last_profile_draws=0,last_profile_copies=0,last_audio_frames=0;
    int stalled = 0, boot_seconds = 0, result = 0;
    bool close = false;
    std::shared_ptr<const gx::FramePacket> pending;
    std::deque<std::shared_ptr<const gx::FramePacket>> scene_commands;
    uint64_t render_serial=0,pending_serial=0;
    auto overlay_due=std::chrono::steady_clock::now(),overlay_last=std::chrono::steady_clock::time_point{};
    std::vector<double> overlay_intervals; overlay_intervals.reserve(1<<16);   // ms between the reticle layer's idle presents, summarised at exit
    std::chrono::steady_clock::duration overlay_period=std::chrono::microseconds(16667);   // the display's refresh interval
    if (network) {
        if (const auto* mode=SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window.get())); mode && mode->refresh_rate>=24.f) overlay_period=std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0/mode->refresh_rate));
        renderer.set_reticle_layer(!std::getenv("UC_RETICLE_LAYER") || std::string_view(std::getenv("UC_RETICLE_LAYER"))!="0");
    }
    uc::FrameLimit frame_limit(options.fps_limit);
    std::chrono::steady_clock::time_point last_present_at{};    // The session CSV is automatic; UC_FRAME_LOG retains its explicit override.
    std::unique_ptr<uc::FrameLog> frame_log;
    try {
        const char* path=std::getenv("UC_FRAME_LOG");
        if(path) frame_log=std::make_unique<uc::FrameLog>(fs::path(path));
        else if(!uc::diagnostic_directory().empty())frame_log=std::make_unique<uc::FrameLog>(uc::diagnostic_directory()/"frames.csv");
        else if(!kPublicRelease && fs::exists(exe_dir()/"frame_log.on")) frame_log=std::make_unique<uc::FrameLog>(exe_dir()/"debug"/("frames-"+std::to_string(process_id())+".csv"));
    } catch(const std::exception& error) {std::fprintf(stderr,"[diagnostics] frame CSV unavailable: %s\n",error.what());}
    uint64_t frame_busy_ns=0,frame_events_ns=0,frame_cpu_last=0,frame_idle_last=0,frame_gpu_busy_last=0,frame_thread_last[8]={}; uint32_t frame_events=0;
#ifdef _WIN32
    const auto thread_cycles=[]() -> uint64_t { ULONG64 cycles=0; QueryThreadCycleTime(GetCurrentThread(),&cycles); return cycles; };
#else
    const auto thread_cycles=[]() -> uint64_t { timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t); return uint64_t(t.tv_sec)*1000000000ull+uint64_t(t.tv_nsec); };   // shortcut: CPU nanoseconds stand in for cycles (see Sched::thread_cycles)
#endif
    uc::FrameRow previous_frame_row; std::array<double,64> recent_intervals{}; size_t recent_intervals_seen=0; double worst_hitch_ms=0; unsigned hitch_lines=0,hitch_second_lines=0,hitches_this_second=0; uint64_t hitch_unlogged=0;
    std::vector<double> present_intervals; present_intervals.reserve(1024);   // ms between presents, reported once per second by --profile
    uint64_t audit_capture_serial=0;
    uint64_t debug_capture_serial=0;
    uint64_t scene_capture_serial=0;
    uint64_t scene_dump_serial=0;   // UC_SCENE_DUMP: screenshot of the sampled frame (scene-<serial>.png in the capture directory)
    uc::PacketDump packet_dump;
    uint32_t scene_capture_pass=0;
    bool scene_capture_started=false;
    bool debug_capture_requested=false;
    bool audit_captured=false;
    bool audit_late_captured=false;
    bool audit_seen_geometry=false;
    uint64_t control_sequence=0;
    uint64_t control_consumed=0;
    auto next_control=start;
    // Takes the next guest frame, renders it and retires finished ones. Called from the loop and, while Windows runs its modal
    // move/resize loop inside SDL_PollEvent, from the event watch below, so the guest keeps running while the window is dragged.
    bool at_frame_boundary=false;   // the last rendered packet presented: a renderer restart starts the next frame cleanly
    bool overlay_idle=false;   // set by service_overlay: the loop should wake in time for the next refresh
    auto service_overlay=[&] {
        overlay_idle=false;   // the reticle layer has a picture to show again before the next tick
        if (network && !pending && renderer.reticle_layer_ready() && (!renderer.held_presented() || renderer.held_has_reticle()) && (frame_limit.interval().count()==0 || frame_limit.interval()<=overlay_period)) {
            // Between the simulation's ticks the held picture is shown again with the reticle re-aimed, once per display refresh, on a fixed cadence
            // (a fixed step, not "now + period", or the rate would wobble with the loop's wake-ups). A frame limit below the refresh rate switches this off.
            overlay_idle=true;
            auto now=std::chrono::steady_clock::now();
            if (now+std::chrono::microseconds(1500)>=overlay_due) {
                if (now<overlay_due) { precise_sleep_until(overlay_due); now=std::chrono::steady_clock::now(); }
                if (renderer.render_overlay(render_serial+1)) {
                    ++render_serial;
                    if (overlay_last!=std::chrono::steady_clock::time_point{} && overlay_intervals.size()<overlay_intervals.capacity()) overlay_intervals.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-overlay_last).count());
                    overlay_last=std::chrono::steady_clock::now();
                    overlay_due+=overlay_period;
                    if (overlay_due<now) overlay_due=now+overlay_period;   // fell behind (a long frame): start over instead of presenting a burst
                }
            }
        }
    };
    auto render_pending = [&]() -> bool {
        bool progressed = false;
        service_overlay();   // the refresh cadence also holds between the packets of a drained burst
        if (!pending) {
            if(scene_commands.empty() && (!packets.detached() || frame_limit.ready(true,std::chrono::steady_clock::now()))) {
                auto next=packets.try_take();
                if(next && next->scene) {
                    auto commands=gx::scene_commands(*next->scene,[&](const auto& texture,bool previous){return renderer.copy_ready(texture,previous);});
                    scene_commands.assign(std::make_move_iterator(commands.begin()),std::make_move_iterator(commands.end()));
                } else pending=std::move(next);
            }
            if(!scene_commands.empty()) { pending=std::move(scene_commands.front()); scene_commands.pop_front(); }
            if(pending)pending_serial=packets.detached() ? ++render_serial : pending->serial;
        }
        // Keep the captured slot alive until its fence completes and readback finishes.
        if (pending && !debug_capture_serial && !audit_capture_serial && !scene_capture_serial && !scene_dump_serial && (network || pending->replay || frame_limit.ready(pending->present,std::chrono::steady_clock::now()))) {
            if (network && pending->present && !pending->replay && last_present_at!=std::chrono::steady_clock::time_point{} && frame_limit.ready(true,std::chrono::steady_clock::now())) {
                // Present pacing: a step's picture is ready whenever its guest work happens to end, which varies by several milliseconds from
                // step to step. Hold an early one back so presents keep the simulation's spacing (a late one cannot be sped up); at most 4 ms.
                const auto due=last_present_at+std::chrono::nanoseconds(1000000000/std::max<uint32_t>(1,network->tick()))-std::chrono::microseconds(300);
                if (const auto now=std::chrono::steady_clock::now(); now<due) std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(due-now,std::chrono::milliseconds(4)));
            }
            const double tail=options.reticle.tail_length/100.0;   // the menu's wake length
            if (network) renderer.set_wake_seconds(tail*4.0/std::max<uint32_t>(1,network->tick()));   // the game draws its wake through the last four simulated pointers
            else renderer.set_wake_seconds(tail*4.0/30);   // single-player: four of the game's own 30 Hz steps, whatever the frame rate (a per-frame length shrinks to nothing at 240 FPS)
            const auto render_start=std::chrono::steady_clock::now();
            const bool display=!pending->replay && frame_limit.ready(pending->present,render_start);
            const auto prior_presents=renderer.presentation_count();
            const bool rendered=renderer.render(*pending,display,pending_serial);
            profile_retries+=!rendered;
            const auto render_elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-render_start).count();
            render_ns+=render_elapsed; last_render_ms=render_elapsed/1e6;
            if (rendered) {
            progressed = true;
            // GPU slots retain immutable inputs until their fences complete; detached GX already completed logically.
            // Offline real-time play releases GX at submission; deterministic playback retains fence completion.
            static const bool finish_at_submit=(!deterministic || network) && !std::getenv("UC_FINISH_AT_GPU");
            if (finish_at_submit && !packets.detached()) uc::gx_completed(pending_serial);
            uint64_t shown_serial=pending_serial;   // what a debug capture reads: with the reticle layer, the overlay frame that presents this packet's picture
            if (network && renderer.reticle_layer_ready() && !renderer.held_presented() && renderer.render_overlay(render_serial+1)) {
                shown_serial=++render_serial;
                // This present is one of the refresh slots: when the next idle slot is due within half a period, it is this one, so the presents keep the display's rate instead of
                // the display's rate plus one per tick.
                if (const auto at=std::chrono::steady_clock::now(); overlay_due<=at+overlay_period/2) overlay_due=std::max(overlay_due+overlay_period,at+overlay_period/2);
            }
            packet_dump.packet(*pending);
            if(options.capture_scene_passes && !scene_capture_started) scene_capture_started=std::any_of(pending->draws.begin(),pending->draws.end(),[](const auto& draw){return draw.callsite==0x80366E14;});
            if(options.capture_scene_passes && scene_capture_started && scene_capture_pass<24) {
                scene_capture_serial=pending_serial; ++scene_capture_pass;
                std::fprintf(stderr,"[scene-pass] %u serial %llu draws %zu present %u clear %u\n",scene_capture_pass,(unsigned long long)pending->serial,pending->draws.size(),pending->present,pending->clear_after_copy);
                if(pending->texture_copy) {
                    const auto& copy=*pending->texture_copy;
                    std::fprintf(stderr,"[scene-pass-copy] source %u:%u:%u:%u target %u:%u\n",copy.source[0],copy.source[1],copy.source[2],copy.source[3],copy.target->levels[0].width,copy.target->levels[0].height);
                }
                if(!pending->draws.empty()) {
                    const auto& draw=pending->draws.back();
                    std::fprintf(stderr,"[scene-pass-last] callsite %08X vertices %zu stages %u blend %u:%u:%u depth %u\n",draw.callsite,draw.vertex_view().size(),draw.material.stages,draw.blend.mode,draw.blend.source,draw.blend.destination,draw.uniforms->pixel[1]);
                    for(uint32_t i=0;i<draw.material.stages;++i) {
                        const auto& stage=draw.material.stage[i];
                        std::fprintf(stderr,"[scene-pass-stage] %u color %06X alpha %06X order %03X konst %03X\n",i,stage.color,stage.alpha,stage.order,stage.konst);
                    }
                    for(uint32_t i=0;i<4;++i) {
                        const auto& r=draw.uniforms->registers[i]; const auto& k=draw.uniforms->konst[i];
                        std::fprintf(stderr,"[scene-pass-color] %u reg %.6g:%.6g:%.6g:%.6g konst %.6g:%.6g:%.6g:%.6g\n",i,r[0],r[1],r[2],r[3],k[0],k[1],k[2],k[3]);
                    }
                }
            }
            if (test_server && draws_wanted) {   // every rendered packet since the request: scene passes precede the presented copy
                for (auto stat : uc::summarize_draws(*pending)) { stat.pass = draws_pass; draw_stats.push_back(stat); }
                ++draws_pass;
                if (draw_projection.empty()) draw_projection = uc::projection_json(uc::scene_projection(*pending));
                if (pending->present) { test_server->draws_ready(uc::test_draws_json(draw_stats), draw_projection.empty() ? "[]" : draw_projection); draws_wanted=false; draws_pass=0; draw_stats.clear(); draw_projection.clear(); }
            }
            if (renderer.presentation_count()!=prior_presents) {
                const auto presented_at=std::chrono::steady_clock::now();
                if (last_present_at!=std::chrono::steady_clock::time_point{} && present_intervals.size()<present_intervals.capacity()) present_intervals.push_back(std::chrono::duration<double,std::milli>(presented_at-last_present_at).count());
                if (frame_log && last_present_at!=std::chrono::steady_clock::time_point{}) {
                    const auto depth=packets.depth(); const auto timings=renderer.last_timings(); const uint64_t cpu=thread_cycles();
                    uc::FrameRow row;
                    row.t_ms=std::chrono::duration<double,std::milli>(presented_at-start).count(); row.interval_ms=std::chrono::duration<double,std::milli>(presented_at-last_present_at).count();
                    row.busy_ms=frame_busy_ns/1e6; row.acquire_ms=timings.acquire_ms; row.submit_ms=timings.submit_ms; row.present_ms=timings.present_ms;
                    row.events_ms=frame_events_ns/1e6; row.events=frame_events; row.cpu_mcycles=frame_cpu_last ? (cpu-frame_cpu_last)/1e6 : 0;
                    { const uint64_t idle=sched.idle_ns(); row.idle_ms=(idle-frame_idle_last)/1e6; frame_idle_last=idle;
                      for(int t=0;t<8;++t) { const uint64_t run=sched.thread_run_ns(t+1); row.thread_ms[t]=(run-frame_thread_last[t])/1e6; frame_thread_last[t]=run; } }
                    row.retraces=uc::vi_stats().retraces; row.textures=renderer.texture_creations(); row.decodes=uc::gx_stats().texture_decodes; row.dvd_reads=uc::dvd_stats().reads;
                    row.audio_blocks=uc::audio_stats().frames; row.seal_wait_ns=gx::seal_wait_ns(); row.switches=sched.switch_stats().switches; row.queued=uint32_t(depth.queued); row.outstanding=uint32_t(depth.outstanding);
                    { const auto window=gx::take_latency_window(); row.sealed_to_done_ms=window.sealed_to_done_ms; row.taken_to_done_ms=window.taken_to_done_ms; row.gpu_ms=renderer.take_gpu_window_ms();
                      { const uint64_t busy=renderer.gpu_busy_ns(); row.gpu_sum_ms=busy>=frame_gpu_busy_last ? (busy-frame_gpu_busy_last)/1e6 : 0; frame_gpu_busy_last=busy; }   // a restarted renderer counts from zero
                      row.gpu_upscale_ms=renderer.pass_ms(uc::GpuPass::upscale); row.gpu_smaa_ms=renderer.pass_ms(uc::GpuPass::smaa); row.gpu_occlusion_ms=renderer.pass_ms(uc::GpuPass::occlusion);
                      row.gpu_rt_as_ms=renderer.take_pass_total_ms(uc::GpuPass::rt_as); row.gpu_rt_shadows_ms=renderer.take_pass_total_ms(uc::GpuPass::rt_shadows); row.gpu_rt_reflections_ms=renderer.take_pass_total_ms(uc::GpuPass::rt_reflections); row.gpu_rt_lighting_ms=renderer.take_pass_total_ms(uc::GpuPass::rt_lighting); row.gpu_rt_path_ms=renderer.take_pass_total_ms(uc::GpuPass::rt_path);
                      const auto rt=renderer.rt_stats(); row.rt_blas=rt.blas; row.rt_triangles=rt.triangles; }
                    frame_log->add(row);
                    {   // a drop in a user log names its cause; 12 lines a second and 400 a session at most
                        double typical_frame_ms=0;   // the median of the last 64 frames; none until 32 are known
                        if(recent_intervals_seen>=32) {const size_t known=std::min<size_t>(recent_intervals_seen,64);auto sorted=recent_intervals;std::nth_element(sorted.begin(),sorted.begin()+known/2,sorted.begin()+known);typical_frame_ms=sorted[known/2];}
                        recent_intervals[recent_intervals_seen++%64]=row.interval_ms;
                        auto hitch=uc::hitch_report(row,previous_frame_row,typical_frame_ms);
                        if(!hitch.empty()) {
                            hitch+=" | "+uc::dvd_area();   // only for a slow frame: the area takes a lock the disc thread also uses
                            if(hitch_lines<400 && hitch_second_lines<12) {std::fprintf(stderr,"%s\n",hitch.c_str());++hitch_lines;++hitch_second_lines;}
                            else ++hitch_unlogged;
                            ++hitches_this_second; worst_hitch_ms=std::max(worst_hitch_ms,row.interval_ms);
                        }
                        previous_frame_row=row;
                    }
                    frame_cpu_last=cpu; frame_busy_ns=frame_events_ns=0; frame_events=0;
                } else if (frame_log) frame_cpu_last=thread_cycles();
                last_present_at=presented_at; frame_limit.presented(presented_at);
            }
            if (!kPublicRelease && packet_dump.scene_sampled()) scene_dump_serial=pending_serial;
            if (pending->present && display && debug_capture_requested) {
                if (!kPublicRelease) debug_capture_serial=shown_serial;   // no screenshot files in the public release
                debug_capture_requested=false;
            }
            audit_seen_geometry |= !pending->draws.empty();
            const bool late_frame=renderer.presentation_count()>=120 && std::chrono::steady_clock::now()-start>=std::chrono::seconds(15);
            if (options.gx_audit && !audit_late_captured && !audit_capture_serial && audit_seen_geometry && pending->present && display && (!audit_captured || late_frame)) audit_capture_serial=pending_serial;
            at_frame_boundary=pending->present;
            pending.reset();
            }
        }
        for (uint64_t serial : renderer.poll_completions()) {
            if (packets.complete(serial)) uc::gx_completed(serial);
            if(serial==scene_capture_serial) {
                const auto pixels=renderer.capture(serial,true);
                const auto directory=exe_dir()/"debug";
                const auto path=directory/("pass-"+std::to_string(process_id())+"-"+std::to_string(scene_capture_pass)+"-"+std::to_string(serial)+".png");
                try {
                    fs::create_directories(directory); uc::save_png(pixels,path);
                    std::fprintf(stderr,"[scene-pass-capture] %s\n",path.string().c_str());
                } catch(const std::exception& error) { std::fprintf(stderr,"scene pass capture failed: %s\n",error.what()); }
                scene_capture_serial=0;
            }
            if (serial==scene_dump_serial) {
                const auto pixels=renderer.capture(serial);
                const auto directory=options.capture_dir.empty() ? exe_dir()/"debug" : options.capture_dir;
                try { fs::create_directories(directory); uc::save_png(pixels,directory/("scene-"+std::to_string(serial)+".png")); }
                catch(const std::exception& error) { std::fprintf(stderr,"scene dump screenshot failed: %s\n",error.what()); }
                scene_dump_serial=0;
            }
            if (serial==debug_capture_serial) {
                const auto pixels=renderer.capture(serial);
                if (!pixels.width || !pixels.height || pixels.rgba.size()!=size_t(pixels.width)*pixels.height*4) throw std::runtime_error("invalid debug-frame readback");
                const auto directory=options.capture_dir.empty() ? exe_dir()/"debug" : options.capture_dir;
                const auto stamp=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                const auto path=directory/("frame-"+std::to_string(stamp)+"-"+std::to_string(process_id())+"-"+std::to_string(serial)+".png");
                try {
                    fs::create_directories(directory);
                    uc::save_png(pixels,path);
                    static const bool with_motion=std::getenv("UC_CAPTURE_MOTION")!=nullptr;   // diagnostic: the last scene's colour and motion target next to the capture
                    if (with_motion) {
                        const auto motion=renderer.capture_motion();
                        uc::save_png(renderer.capture_scene(),fs::path(path).replace_extension(".scene.png"));
                        if (std::FILE* file=uc::fopen_path(fs::path(path).replace_extension(".motion"),"wb")) {
                            std::fwrite(&motion.width,4,1,file); std::fwrite(&motion.height,4,1,file);
                            std::fwrite(motion.rg.data(),sizeof(float),motion.rg.size(),file); std::fclose(file);
                        }
                    }
                    std::fprintf(stderr,"debug screenshot: %s\n",path.string().c_str());
                    if(test_server)test_server->captured(path.string());
                } catch(const std::exception& error) {
                    std::fprintf(stderr,"debug screenshot failed: %s\n",error.what());
                    if(test_server)test_server->capture_failed(error.what());
                }
                debug_capture_serial=0;
            }
            if (options.gx_audit && serial==audit_capture_serial) {
                const auto pixels=renderer.capture(serial);
                if (!pixels.width || !pixels.height || pixels.rgba.size()!=size_t(pixels.width)*pixels.height*4) throw std::runtime_error("invalid game-frame readback");
                const auto path=exe_dir()/(audit_captured ? "gx-later-frame.png" : "gx-first-geometry.png");
                uc::save_png(pixels,path);
                std::fprintf(stderr,"gx audit capture: serial %llu %ux%u %s\n",static_cast<unsigned long long>(serial),pixels.width,pixels.height,path.string().c_str());
                audit_late_captured=audit_captured;
                audit_captured=true;
                audit_capture_serial=0;
            }
        }
        return progressed;
    };
    // Upscaler kind or frame generation switch: the device depends on them, so the renderer is replaced between two frames. Every submitted packet
    // is completed first (the guest waits for them); the EFB copies the old renderer held are drawn blank until the game copies them again.
    const auto restart_renderer=[&] {
        const auto began=std::chrono::steady_clock::now();
        do { for (uint64_t serial : renderer.poll_completions()) if (packets.complete(serial)) uc::gx_completed(serial); }
        while (renderer.wait_for_pending(1000000000ull));
        auto lost=renderer.copy_keys();
        renderer_storage.reset(); renderer_storage.emplace(options.renderer_backend);
        init_renderer(false);
        renderer.lost_copies(std::move(lost));
        std::fprintf(stderr,"Renderer restarted in %.0f ms: upscaler %s, frame generation %s\n",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count(),
            std::string(uc::upscaler_name(renderer.active_upscaler())).c_str(),renderer.frame_generation_active() ? "on" : "off");
    };
    // In-game settings menu (Insert) and pause (P). The menu shows and saves config.json, the run's command-line overrides stay as they are.
    uc::HostKeys host_keys;
    uc::SteamOverlay steam_overlay;   // Steam Deck: Steam's own menus (Steam button, "...") are on top: the game gets no input meanwhile
    bool steam_overlay_open=false;
    std::vector<SDL_Scancode> menu_keys;   // the keys bound to the menu action (empty: Insert, Tab on macOS)
    std::vector<SDL_Scancode> speed_keys;   // the keys bound to the speed_up action (empty: no speed key)
    const auto set_key_lists=[&](const uc::Bindings& bindings) {
        for(const auto [action,keys]:{std::pair{uc::Action::menu,&menu_keys},std::pair{uc::Action::speed_up,&speed_keys}}) {
            keys->clear();
            for(const auto& binding:bindings[size_t(action)]) if(binding.kind==uc::BindingKind::physical && binding.input.kind==uc::PhysicalKind::key) keys->push_back(SDL_Scancode(binding.input.code));
        }
    };
    set_key_lists(input_config.input);
    uint64_t menu_click_presents=0;
    std::deque<SDL_Event> menu_click_events;   // harness menu clicks: press and release, one per main-loop iteration
    bool restart_pending=false;   // apply upscaling changes at the next frame boundary; replace the device only when required
    std::optional<uc::LiveSettings> live_settings;
    std::optional<uc::SettingsMenu> settings_menu;
    std::optional<uc::DeckMenu> deck_menu;   // the Steam Deck's own menu (View button): replaces the desktop menu on Linux and pauses the game while open
    std::shared_ptr<std::promise<std::string>> features_promise; std::shared_future<std::string> features_query; std::optional<uc::FeatureReport> features;
    if (!options.headless) {
        const auto* desktop=SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        const uint32_t monitor_width=desktop ? uint32_t(desktop->w) : options.width, monitor_height=desktop ? uint32_t(desktop->h) : options.height;
        uc::SettingsTargets targets;
        targets.render_scale=[&renderer,&options,monitor_width,monitor_height](double scale) {
            options.render_scale=scale ? scale : uc::monitor_render_scale(monitor_width,monitor_height);   // a renderer restart starts from it
            renderer.set_render_scale(options.render_scale);
        };
        targets.upscaling=[&](const uc::Options& next) {
            restart_pending=true;
            options.upscaler=next.upscaler; options.upscale_mode=next.upscale_mode; options.sharpening=next.sharpening; options.sharpness=next.sharpness;
            options.frame_generation=next.frame_generation && (!deterministic || options.netplay); options.frame_generation_multiplier=next.frame_generation_multiplier;
        };
        targets.fps_limit=[&](std::optional<uint32_t> limit) {
            options.fps_limit=limit; frame_limit.set_limit(limit);
            if(!network)uc::game_pacing_set_mode(uc::pacing_for_limit(limit,options.game_hz,options.unlock_60fps,server_mode && !options.deterministic));
        };
        targets.bloom=[&](const uc::BloomSettings& settings) { options.bloom=settings.enabled; options.bloom_strength=settings.strength; renderer.set_bloom(settings); };
        targets.crosshair_scale=[&](uint32_t percent) { options.crosshair_scale=percent; renderer.set_crosshair_scale(percent); };
        targets.reticle=[&](const uc::ReticleOptions& reticle) { options.reticle=reticle; gx::set_reticle_style(reticle_style_of(reticle)); };
        targets.mouse_sensitivity=[&](uint32_t percent) { options.mouse_sensitivity=percent; host_input.sensitivity(percent); coop.sensitivity(percent); };
        targets.rumble=[&](uint32_t percent) { options.rumble=percent; };
        targets.pointer=[&](uc::PointerMode mode,uint32_t smoothing) { options.pointer=mode; options.pointer_smoothing=smoothing; host_input.pointer(mode); input.pointer_smoothing(smoothing/100.f); input2.pointer_smoothing(smoothing/100.f); capture_pointer(!host_keys.menu_open); };   // the in-game menu is open while it changes this, but the harness changes it during play: the cursor changes hands now
        targets.reload_zone=[&](uc::ReloadZone zone,uint32_t size) { options.reload_zone=zone; options.reload_zone_size=size; input.reload_zone(zone,size/100.f); input2.reload_zone(zone,size/100.f); };
        targets.hud_position=[&](uc::HudPosition position) { options.hud_position=position; view_for_output(view_width,view_height); };
        targets.hdr=[&](uc::HdrSettings hdr) {
            if(fixed_hdr_levels) { hdr.peak_nits=options.hdr.peak_nits; hdr.paper_white_nits=options.hdr.paper_white_nits; }
            options.hdr=hdr; renderer.set_hdr(hdr);
        };
        targets.effects=[&](const uc::GraphicsEffects& effects) { options.effects=effects; renderer.set_effects(effects); };
        targets.dlss5=[&](const uc::Dlss5Settings& settings) { options.dlss5=settings; renderer.set_dlss5(settings); };
        targets.prompt_style=[&](uc::PromptStyle style) { options.prompt_style=style; renderer.set_prompt_style(style); };
        targets.bindings=[&](const uc::Bindings& bindings) { input.bindings(bindings); uc::prompt_hooks_set_bindings(bindings); set_key_lists(bindings); };
        targets.gamepad=[&](const uc::GamepadSettings& settings) { options.gamepad=settings; input.pad_settings(settings); refresh_prompt_device(); };
        targets.wiimote=[&](const uc::WiimoteSettings& settings) { options.wiimote=settings; input.wiimote_settings(settings); input2.wiimote_settings(settings); refresh_prompt_device(); };
        targets.player2=[&](const uc::CoopSettings& settings) { options.player2=settings; input2.bindings(settings.bindings); input2.pad_settings(settings.gamepad); };
        live_settings.emplace(config_path,input_config,std::move(targets));
        if (controlled && options.renderer_backend==uc::RendererBackend::vulkan) { features=uc::FeatureReport{true,true,true,true,4,{},{},{},{},true,{},16}; features->dlss5=true; }   // Vulkan test runs exercise every integrated choice whatever this PC has
        else {   // what this PC supports, asked of uc.exe --features (the launcher's query) while the game runs; a hung query is left behind at exit
            features_promise=std::make_shared<std::promise<std::string>>(); features_query=features_promise->get_future().share();
            std::thread([promise=features_promise,exe=exe_path(),config=config_path,backend=options.renderer_backend] { promise->set_value(uc::query_features(exe,config,30000,backend)); }).detach();
        }
        const auto config_language=uc::launcher::config_language(input_config);
        settings_menu.emplace(*live_settings,config_language=="system" ? uc::launcher::system_language() : config_language,[&]() -> const uc::FeatureReport* {
            if (!features && features_query.valid() && features_query.wait_for(std::chrono::seconds(0))==std::future_status::ready) features=uc::feature_report_from_json(features_query.get());
            return features ? &*features : nullptr;
        },monitor_width,monitor_height,options.renderer_backend);
#ifndef _WIN32
        if(deck_view) deck_menu.emplace(*live_settings,config_language=="system" ? uc::launcher::system_language() : config_language,[&]() -> const uc::FeatureReport* {
            if (!features && features_query.valid() && features_query.wait_for(std::chrono::seconds(0))==std::future_status::ready) features=uc::feature_report_from_json(features_query.get());
            return features ? &*features : nullptr;
        },monitor_width,monitor_height);
#endif
        settings_menu->set_mod_menu(&mod_menu);
        if(deck_menu) deck_menu->set_mods({[&mod_menu] { return mod_menu.registry(); },
            [&mod_menu](const std::string& id,const std::function<void(uc::mods::ModSettings&)>& edit) { return mod_menu.edit_settings(id,edit); },
            [&mod_menu](const std::string& id) { return mod_menu.dll_hash(id); }},[&mod_menu] { mod_menu.registry_changed(); });   // the Deck menu's Mods page: the launcher's rows over the live mods
        settings_menu->set_pad_status([&] { return gamepad ? gamepad->status() : uc::PadStatus{}; });
        if(gamepad) gamepad->capture_sink([&](uc::PhysicalInput pad_input,bool down) { if(deck_menu) deck_menu->pad_capture(pad_input,down); else settings_menu->pad_capture(pad_input,down); });   // the menu's gamepad window waits for a pad press
        settings_menu->set_wii_status([&] {   // the first player's remote may be either reader's: connected when one is
            uc::WiimoteStatus status=wiimote ? wiimote->status() : uc::WiimoteStatus{};
            if(wiimote2) { const auto other=wiimote2->status(); status.connected|=other.connected; status.has_nunchuk|=other.has_nunchuk; if(status.error.empty()) status.error=other.error; }
            return status;
        });
        settings_menu->set_second_command([&](uc::SettingsMenu::SecondCommand command) {   // local two-player play, from the menu's commit(): the answer to the new-device window and the disconnect button
            switch(command) {
            case uc::SettingsMenu::SecondCommand::disconnect: coop.release_second(); break;
            case uc::SettingsMenu::SecondCommand::first: coop.answer_first(); break;
            case uc::SettingsMenu::SecondCommand::second: coop.answer_second(); break;
            case uc::SettingsMenu::SecondCommand::not_now: coop.cancel_pending(); break;
            }
        });
        if(wiimote) wiimote->capture_sink([&](uc::PhysicalInput remote_input,bool down) { settings_menu->wii_capture(remote_input,down); });   // the Wii Remote window waits for a button
        if(wiimote2) wiimote2->capture_sink([&](uc::PhysicalInput remote_input,bool down) { settings_menu->wii_capture(remote_input,down); });
        mod_menu.set_save([&live_settings](const nlohmann::ordered_json& section) { live_settings->apply_mods(section); });   // the config's "mods" section
        if(!kPublicRelease)settings_menu->set_open_debug([] {   // the DLSS 5 tab's Open Debug: the debug folder with ngx_dlss5.log
            const auto folder=exe_dir()/"debug"; std::error_code ignored; std::filesystem::create_directories(folder,ignored);
            uc::open_folder(folder);
        });
        settings_menu->set_dlss5_status([&renderer]() { return uc::SettingsMenu::Dlss5Report{renderer.dlss5_active(),renderer.dlss5_status()}; });   // why the pass is off in this run (the report only says what the GPU could do)
    }
    // Menu / pause state → renderer, mouse, game input, audio. Called after every host key and after a renderer restart.
    const auto apply_host_keys=[&] {
        if (options.headless) return;
        static bool menu_was_open=false;
        if (deck_menu && host_keys.menu_open && !menu_was_open) deck_menu->opened();
        menu_was_open=host_keys.menu_open;
        renderer.set_menu(!host_keys.menu_open ? std::function<void()>() : deck_menu ? std::function<void()>([&] { deck_menu->build(); }) : std::function<void()>([&] { settings_menu->build(); }),
                          [&] { if (deck_menu) deck_menu->setup(); else settings_menu->setup(); });
        if (!host_keys.menu_open && settings_menu) settings_menu->menu_closed();   // a binding capture waiting for a pad press must not outlive the menu
        if (!host_keys.menu_open && deck_menu) deck_menu->menu_closed();
        const bool halted=host_keys.paused || (deck_menu && host_keys.menu_open && !network);   // the Deck menu pauses the game (network play cannot pause)
        uc::game_pacing_set_paused(halted);
        static bool audio_paused=false;   // only a real change: resuming drops the queued audio (a click when the menu merely opens)
        if (audio && audio_paused!=halted) { audio->pause(halted); audio_paused=halted; }
        const bool playing=!host_keys.menu_open && !host_keys.paused && !steam_overlay_open;
        capture_pointer(!host_keys.menu_open);   // the menu needs the cursor
        const bool game_focus=playing && (controlled || (SDL_GetWindowFlags(window.get())&SDL_WINDOW_INPUT_FOCUS)!=0);
        input.focus(game_focus);   // nothing reaches the game under the menu or while paused
        input2.focus(game_focus);
    };
    const auto host_key_pressed=[&](SDL_Scancode key) {   // true when the key belongs to the port (never to the game)
        if(network && key==SDL_SCANCODE_P)return true; // Escape sends the original synchronized pause action.
        const auto result=uc::host_key(host_keys,key,settings_menu && settings_menu->capturing(),menu_keys);
        if (result==uc::HostKeyResult::none) return false;
        if (result!=uc::HostKeyResult::consumed) {
            if (result==uc::HostKeyResult::pause_toggled) std::fprintf(stderr,"Game %s\n",host_keys.paused ? "paused" : "resumed");
            apply_host_keys();
        }
        return true;
    };
    if (deck_menu) deck_menu->set_close([&] { if (host_keys.menu_open) { host_keys.menu_open=false; apply_host_keys(); } });
    if (deck_menu) deck_menu->set_quit([&] { close=true; });   // the Quit game page: leaves the main loop like closing the window
    // One loop iteration without event polling: harness commands, input services, rendering. Also run from the event watch,
    // so the harness (and the bot) keep working while Windows runs its modal drag/resize loop.
    auto service_frame = [&] {
        if(network && network->waiting()) {
            if(!network_wait_screen) {
                network_wait_screen.emplace(menu_language);
                network_wait_screen->wait_for_peer(uc::launcher::text(menu_language,"network_waiting"));
                std::fprintf(stderr,"[netplay] waiting screen shown\n");
            }
        } else if(network_wait_screen) {
            network_wait_screen.reset();
            std::fprintf(stderr,"[netplay] waiting screen closed\n");
        }
        if(test_server) {
            const auto state=uc::test_state_latest();
            auto frame_json=uc::test_frame_json(state,uc::clock_speed(),last_render_ms,renderer.presentation_count(),uc::vi_stats().retraces,test_pipes->dropped());
            frame_json.pop_back(); frame_json+=std::string(",\"menu_open\":")+(host_keys.menu_open ? "true" : "false")+",\"paused\":"+(host_keys.paused ? "true" : "false")
                +",\"upscaler\":\""+std::string(uc::upscaler_name(renderer.active_upscaler()))+"\",\"frame_generation\":"+(renderer.frame_generation_active() ? "true" : "false")
                +",\"aim\":["+std::to_string(input.latest_aim()[0])+","+std::to_string(input.latest_aim()[1])+"]"   // the game's pointer (x right, y down): the window's edge is at .8 across and .6 down
                +",\"aim2\":["+std::to_string(input2.latest_aim()[0])+","+std::to_string(input2.latest_aim()[1])+"]"   // the second player's pointer
                +",\"game_seconds\":"+std::to_string(uc::game_pacing_stats().timestep_ns/1e9)
                +",\"simulation_seconds\":"+std::to_string((network ? network->simulation_ns() : uc::game_pacing_stats().timestep_ns)/1e9)
                +",\"network_waiting\":"+(network_wait_screen ? "true" : "false")
                +",\"sample_time_ns\":"+std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count())
                +",\"fps_limit\":"+std::to_string(options.fps_limit.value_or(240))+"}";
            const auto pumped=test_server->pump(state.frame,frame_json);
            debug_capture_requested|=pumped.capture; draws_wanted|=pumped.draws;
            if (!menu_click_events.empty() && host_keys.menu_open && renderer.presentation_count()!=menu_click_presents) {   // one event per presented frame: the menu needs the hover of a frame before the press
                menu_click_presents=renderer.presentation_count(); renderer.menu_event(menu_click_events.front()); menu_click_events.pop_front();
            }
            for (const auto code : pumped.host_keys) host_key_pressed(SDL_Scancode(code));
            if (host_keys.menu_open) for (const auto& [x,y] : pumped.menu_clicks) {   // harness clicks on the menu: the pointer moves now, the press and the release follow on the next iterations (ImGui needs the hover first)
                SDL_Event motion{}; motion.type=SDL_EVENT_MOUSE_MOTION; motion.motion.windowID=SDL_GetWindowID(window.get()); motion.motion.x=x; motion.motion.y=y;
                SDL_Event press{}; press.type=SDL_EVENT_MOUSE_BUTTON_DOWN; press.button.windowID=motion.motion.windowID; press.button.button=SDL_BUTTON_LEFT; press.button.down=true; press.button.clicks=1; press.button.x=x; press.button.y=y;
                SDL_Event release=press; release.type=SDL_EVENT_MOUSE_BUTTON_UP; release.button.down=false;
                renderer.menu_event(motion); menu_click_events.push_back(motion); menu_click_events.push_back(press); menu_click_events.push_back(release);
            }
            for (const auto& graphics : pumped.options) if (live_settings) {   // the in-game menu's path: merged over the saved settings, applied and saved
                try {
                    uc::ConfigDocument document; uc::store_options(document,live_settings->options());
                    document.properties["graphics"].update(nlohmann::ordered_json::parse(graphics));
                    live_settings->apply(uc::options_from_config(document));
                } catch (const std::exception& error) { std::fprintf(stderr,"[test-server] options rejected: %s\n",error.what()); }
            }
            for (const auto& gamepad_keys : pumped.gamepad) if (live_settings) {   // the same for the "gamepad" object: its keys merged over the saved ones (a nested object such as gyro replaces the saved one)
                try {
                    uc::ConfigDocument document; uc::store_options(document,live_settings->options());
                    document.properties["gamepad"].update(nlohmann::ordered_json::parse(gamepad_keys));
                    live_settings->apply(uc::options_from_config(document));
                    if (live_settings->error()) std::fprintf(stderr,"[test-server] gamepad options rejected: %s\n",live_settings->error()->c_str());
                } catch (const std::exception& error) { std::fprintf(stderr,"[test-server] gamepad options rejected: %s\n",error.what()); }
            }
            for (const auto& settings : pumped.settings) if (live_settings) {   // root keys of config.json (pointer, reload_zone, ...), merged over the saved ones
                try {
                    uc::ConfigDocument document; uc::store_options(document,live_settings->options());
                    document.properties.update(nlohmann::ordered_json::parse(settings));
                    live_settings->apply(uc::options_from_config(document));
                    if (live_settings->error()) std::fprintf(stderr,"[test-server] settings rejected: %s\n",live_settings->error()->c_str());
                } catch (const std::exception& error) { std::fprintf(stderr,"[test-server] settings rejected: %s\n",error.what()); }
            }
            close|=pumped.quit;
            if(test_pipes->disconnected() && !test_released) { test_released=true; test_server->release_all(); }
        }
        if(!server_mode && controlled && std::chrono::steady_clock::now()>=next_control) {
            next_control=std::chrono::steady_clock::now()+std::chrono::milliseconds(50);
            std::ifstream file(options.test_control);
            std::string line;
            if(std::getline(file,line)) if(auto command=uc::parse_test_command(line);command && command->sequence>control_sequence) {
                if(command->kind==uc::TestKind::legacy) {
                    if(!input.apply_test_command(command->sequence,command->x,command->y,command->held))throw std::runtime_error("internal input command rejected");
                } else {
                    uc::HostEventKind kind;
                    switch(command->kind) {
                    case uc::TestKind::key: kind=uc::HostEventKind::key; break;
                    case uc::TestKind::mouse: kind=uc::HostEventKind::mouse; break;
                    case uc::TestKind::wheel: kind=uc::HostEventKind::wheel; break;
                    case uc::TestKind::motion: kind=uc::HostEventKind::motion; break;
                    case uc::TestKind::focus: kind=uc::HostEventKind::focus; break;
                    case uc::TestKind::legacy: throw std::logic_error("unexpected legacy event");
                    }
                    int width=0,height=0;
                    if(!SDL_GetWindowSizeInPixels(window.get(),&width,&height))throw std::runtime_error(SDL_GetError());
                    if(!input.apply_test_event(command->sequence,{kind,command->code,int(command->value),command->x,command->y},uint32_t(std::max(0,width)),uint32_t(std::max(0,height))))throw std::runtime_error("internal host command rejected");
                }
                control_sequence=command->sequence;
                debug_capture_requested|=command->capture;
                std::fprintf(stderr,"[test-control] applied %llu aim %.3f %.3f held %u capture %u\n",(unsigned long long)control_sequence,command->x,command->y,command->held,command->capture);
            }
            const auto consumed=input.last_consumed_sequence();
            if(consumed>control_consumed) {
                control_consumed=consumed;
                std::fprintf(stderr,"[test-control] consumed %llu\n",(unsigned long long)consumed);
            }
        }
        const auto services=input.consume_service_actions();
        debug_capture_requested|=(services.pressed&(uint64_t(1)<<unsigned(uc::Action::screenshot)))!=0;
        if(!controlled && options.window_mode==uc::WindowMode::windowed &&
            (services.pressed&(uint64_t(1)<<unsigned(uc::Action::release_cursor)))) {
            capture_pointer(false);
            input.focus(false);
            input2.focus(false);
        }
        render_pending();
    };
    std::exception_ptr watch_error;
    bool in_render = false;
    struct Watch { std::function<void()>* render; std::function<void(uint32_t,uint32_t)>* resize; SDL_WindowID window; } watch_state;
    // A guest frame is ~2.4 packets (scene passes + copies), 72 packets/s at 30 fps, and the modal loop's timer ticks only 64-100 times a second:
    // one packet per tick starves the guest. So each tick keeps servicing while packets are waiting, for at most 12 ms.
    std::function<void()> render_step = [&] {
        if (in_render) return;
        in_render = true;
        try {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(12);
            do {
                service_frame();
                if (close) break;
                if (pending) std::this_thread::sleep_for(std::chrono::milliseconds(1));        // present spacing of the frame limiter
                else if (!packets.wait_ready(std::chrono::nanoseconds(0))) break;              // nothing left to render
            } while (std::chrono::steady_clock::now() < until);
        } catch (...) { watch_error = std::current_exception(); }
        in_render = false;
    };
    std::function<void(uint32_t,uint32_t)> resize_step = [&](uint32_t w, uint32_t h) { try { renderer.resize(w, h); view_for_output(int(w),int(h)); } catch (...) { watch_error = std::current_exception(); } };
    watch_state = {&render_step, &resize_step, window ? SDL_GetWindowID(window.get()) : 0};
    const auto watch = [](void* user, SDL_Event* event) -> bool {   // runs on the thread that pushes the event: the main one on Windows, also inside the modal resize loop
        auto& state = *static_cast<Watch*>(user);
        if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && event->window.windowID == state.window) (*state.resize)(uint32_t(std::max(0, event->window.data1)), uint32_t(std::max(0, event->window.data2)));
        else if (event->type == SDL_EVENT_WINDOW_EXPOSED) (*state.render)();
        return true;
    };
#ifdef __ANDROID__   // Android pushes window events from its UI (Java) thread, where the watch would call the renderer off its owner thread; the main loop
    (void)watch;     // handles every resize itself, and there is no modal loop to render through
#else
    if (!options.headless) SDL_AddEventWatch(watch, &watch_state);
#endif
    uc::cpu_sampler_start_once("main");
    uc::RumbleRouter rumble_router;
    unsigned rumble_pad_starts=0,rumble_wii_starts=0; uint64_t rumble_game_starts=0,rumble_logged_ms=0;   // the diagnostics line of the vibration
    bool pad_back_held=false;   // View/Back held: with Start it opens and closes the settings menu from a gamepad (no keyboard on a Steam Deck)
    const auto pad_allowed=[&] { return !host_keys.menu_open && !host_keys.paused && !steam_overlay_open && (SDL_GetWindowFlags(window.get())&SDL_WINDOW_INPUT_FOCUS)!=0; };   // what the pads may hand to the game
    const auto sync_overlay=[&] {
        if (controlled || steam_overlay.open()==steam_overlay_open) return;
        steam_overlay_open=!steam_overlay_open;   // Steam's menus opened or closed: nothing reaches the game while they are up, and what was held is let go
        std::fprintf(stderr,"[steam] Steam's menu %s\n",steam_overlay_open ? "opened: the game gets no input" : "closed");
        apply_host_keys();
        if(gamepad) gamepad->update(input,pad_allowed(),&input2);
        coop.update_remotes(pad_allowed(),options.wiimote);
    };
    std::chrono::steady_clock::time_point last_pump{};
    uint64_t last_pump_presents=~uint64_t(0);
    bool coop_all_mice=false;   // Windows: the pointer backend hands over every mouse
    bool coop_was_asking=false,coop_menu_opened=false,coop_menu_was_open=false;   // the question about a new device: the menu it opened, so that it can close it again
    while (!close && !runner.done.load()) {
        ++profile_loops;
        if (!options.headless) {
            sync_overlay();
            if(coop_enabled) {   // what the game is doing now, before this loop's events go through the session: only a press during play joins a second player
                int coop_width=0,coop_height=0;
                SDL_GetWindowSizeInPixels(window.get(),&coop_width,&coop_height);
                const auto context=input.current_context();
                coop.tick({pad_allowed() && (context==uc::InputContext::gameplay || context==uc::InputContext::qte),context==uc::InputContext::gameplay || context==uc::InputContext::qte,pointer_captured(),
                           options.pointer==uc::PointerMode::absolute,SDL_GetTicksNS(),
                           {SDL_GetWindowID(window.get()),uint32_t(std::max(0,coop_width)),uint32_t(std::max(0,coop_height)),SDL_GetWindowPixelDensity(window.get())}});
#ifdef _WIN32
                const bool all_mice=pointer_backend && pointer_backend->all_devices();   // the backend took the legacy mouse messages and SDL's own reading with them: when it lets go, SDL's relative mode registers its own again
                if(coop_all_mice && !all_mice) { capture_pointer(false); capture_pointer(!host_keys.menu_open); }
                coop_all_mice=all_mice;
#endif
            }
            if(settings_menu) {   // the menu's view of local two-player play, and the question about a new device
                const auto status=coop.status(); const auto ask=coop.pending();
                const auto code=[](uc::DeviceKind kind) { return kind==uc::DeviceKind::gamepad ? 1 : kind==uc::DeviceKind::wiimote ? 2 : kind==uc::DeviceKind::pointer ? 3 : 0; };
                if(!controlled) {
                    const SDL_Scancode menu_key=menu_keys.empty() ? uc::kFallbackMenuKey : menu_keys.front();
                    if(ask.asking && !coop_was_asking && !host_keys.menu_open) { host_key_pressed(menu_key); coop_menu_opened=true; }   // a new device was pressed in a game menu: the menu opens with the question
                    if(!ask.asking && coop_menu_opened) { coop_menu_opened=false; if(host_keys.menu_open) host_key_pressed(menu_key); }   // answered: back to the game
                    if(ask.asking && coop_menu_was_open && !host_keys.menu_open) { coop.cancel_pending(); coop_menu_opened=false; }   // the menu was closed with the question open: not now
                }
                coop_was_asking=ask.asking; coop_menu_was_open=host_keys.menu_open;
                settings_menu->set_second_player({second_available,status.joined,status.joined ? code(status.kind) : 0,ask.asking ? code(ask.kind) : 0,ask.replaces,status.problem,coop_menu_opened});   // question_only: the question opened the menu, so only its window shows
            }
            SDL_Event event;
            const auto events_start=frame_log ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
#ifdef _WIN32
            const auto next_event=[&] { return SDL_PollEvent(&event); };
#else
            // The loop turns about ten times per presented frame and SDL_PollEvent pumps the devices on every call. SDL's Deck controller driver sends its lizard-mode watchdog
            // (three HID feature reports, 7-8 ms on the calling thread) every 200 pumps, which made 6% of the frames 8 ms longer. Pump once per presented frame (as SDL assumes), right
            // after the present: the 8 ms then fall into the wait for the next frame (frame limit, GPU) instead of delaying a present. 25 ms without a present pumps too (menus, loading).
            const auto pump_time=std::chrono::steady_clock::now();
            const uint64_t pump_presents=renderer.presentation_count();
            if(pump_presents!=last_pump_presents || pump_time-last_pump>=std::chrono::milliseconds(25)) { last_pump=pump_time; last_pump_presents=pump_presents; SDL_PumpEvents(); }
            const auto next_event=[&] { return SDL_PeepEvents(&event,1,SDL_GETEVENT,SDL_EVENT_FIRST,SDL_EVENT_LAST)>0; };
#endif
            while (next_event()) {
                ++frame_events;
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) close = true;
                if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED && event.window.windowID == SDL_GetWindowID(window.get())) { renderer.resize(uint32_t(std::max(0, event.window.data1)), uint32_t(std::max(0, event.window.data2))); view_for_output(event.window.data1,event.window.data2); }
                if (event.type == SDL_EVENT_WINDOW_HDR_STATE_CHANGED) renderer.hdr_state_changed();   // Windows HDR toggled or the window moved to another display
                if(event.type==SDL_EVENT_KEY_DOWN) coop.note_keyboard();   // a keyboard player aims with a mouse: see CoopSession
#ifdef _WIN32
                coop.sdl_mouse(event);   // SDL's mouse id is the Raw Input device handle there: the mice tell themselves apart while the pointer backend is not listening
#endif
                if(controlled) { if(gamepad) gamepad->event(event,nullptr); continue; }   // with UC_TEST_REAL_PAD the pad opens and streams its gyro like in a player's run, but hands the game nothing
#ifndef _WIN32
                {   // diagnostics (Steam Deck): what the trackpads, the menu's cursor and the Steam overlay send (a few hundred lines per run)
                    static int logged=0; const auto type=event.type;
                    if(logged<1500 && (type==SDL_EVENT_WINDOW_FOCUS_GAINED || type==SDL_EVENT_WINDOW_FOCUS_LOST || type==SDL_EVENT_WINDOW_MOUSE_ENTER || type==SDL_EVENT_WINDOW_MOUSE_LEAVE ||
                                      type==SDL_EVENT_MOUSE_BUTTON_DOWN || type==SDL_EVENT_MOUSE_BUTTON_UP || type==SDL_EVENT_JOYSTICK_BUTTON_DOWN || type==SDL_EVENT_JOYSTICK_BUTTON_UP ||
                                      type==SDL_EVENT_GAMEPAD_BUTTON_DOWN || type==SDL_EVENT_GAMEPAD_BUTTON_UP || type==SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN || type==SDL_EVENT_GAMEPAD_TOUCHPAD_UP || type==SDL_EVENT_KEY_DOWN)) {
                        ++logged;
                        std::fprintf(stderr,"[diag] wall=%lld.%03d ms=%llu menu=%d focus=%d type=0x%x",(long long)std::time(nullptr),0,(unsigned long long)SDL_GetTicks(),host_keys.menu_open ? 1 : 0,(SDL_GetWindowFlags(window.get())&SDL_WINDOW_INPUT_FOCUS) ? 1 : 0,unsigned(type));
                        if(type==SDL_EVENT_MOUSE_BUTTON_DOWN || type==SDL_EVENT_MOUSE_BUTTON_UP) std::fprintf(stderr," mouse button=%d which=%u at %.0f,%.0f",int(event.button.button),unsigned(event.button.which),event.button.x,event.button.y);
                        else if(type==SDL_EVENT_JOYSTICK_BUTTON_DOWN || type==SDL_EVENT_JOYSTICK_BUTTON_UP) std::fprintf(stderr," joystick which=%u button=%d",unsigned(event.jbutton.which),int(event.jbutton.button));
                        else if(type==SDL_EVENT_GAMEPAD_BUTTON_DOWN || type==SDL_EVENT_GAMEPAD_BUTTON_UP) std::fprintf(stderr," gamepad button=%d",int(event.gbutton.button));
                        else if(type==SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN || type==SDL_EVENT_GAMEPAD_TOUCHPAD_UP) std::fprintf(stderr," touchpad=%d finger=%d at %.2f,%.2f",int(event.gtouchpad.touchpad),int(event.gtouchpad.finger),event.gtouchpad.x,event.gtouchpad.y);
                        else if(type==SDL_EVENT_KEY_DOWN) std::fprintf(stderr," key scancode=%d",int(event.key.scancode));
                        std::fputc('\n',stderr);
                    }
                }
#endif
                if(event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type==SDL_EVENT_GAMEPAD_BUTTON_UP) {
                    const bool down=event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                    if(down && (event.gbutton.button==SDL_GAMEPAD_BUTTON_GUIDE || event.gbutton.button==SDL_GAMEPAD_BUTTON_MISC1)) { steam_overlay.hint(); sync_overlay(); }   // the Steam / "..." button reaches the game before its menu shows
                    if(deck_menu && event.gbutton.button==SDL_GAMEPAD_BUTTON_BACK) {   // View opens and closes the Deck menu
                        if(down && !settings_menu->capturing()) host_key_pressed(menu_keys.empty() ? uc::kFallbackMenuKey : menu_keys.front());
                        continue;
                    }
                    if(event.gbutton.button==SDL_GAMEPAD_BUTTON_BACK) pad_back_held=down;
                    else if(down && event.gbutton.button==SDL_GAMEPAD_BUTTON_START && pad_back_held && !settings_menu->capturing()) {
                        host_key_pressed(menu_keys.empty() ? uc::kFallbackMenuKey : menu_keys.front());   // the same toggle as the menu key; the Start press is not passed on (no pause)
                        continue;
                    }
                }
                if(gamepad) gamepad->event(event,&input,&input2);   // keeps its own state; it hands the game nothing while a menu is open (see update below); the second player's pad feeds input2
                if(event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN || (event.type==SDL_EVENT_GAMEPAD_AXIS_MOTION && std::abs(int(event.gaxis.value))>16000)) {pad_last_used=true;wii_last_used=false;}
                else if(pad_last_used && (event.type==SDL_EVENT_KEY_DOWN || event.type==SDL_EVENT_MOUSE_BUTTON_DOWN || event.type==SDL_EVENT_MOUSE_MOTION)) {
                    static int shown=0;   // diagnostics: who switched the prompts from the pad to the mouse and keyboard (a few lines per run)
                    if(shown++<12) {
                        const bool mouse=event.type!=SDL_EVENT_KEY_DOWN;
                        const SDL_KeyboardID key_source=event.key.which; const SDL_MouseID mouse_source=event.motion.which;
                        const char* name=mouse ? SDL_GetMouseNameForID(mouse_source) : SDL_GetKeyboardNameForID(key_source);
                        std::fprintf(stderr,"[input] prompts left the pad for %s id=%u \"%s\"%s\n",mouse ? "the mouse" : "the keyboard",mouse ? unsigned(mouse_source) : unsigned(key_source),name ? name : "?",
                                     event.type==SDL_EVENT_MOUSE_MOTION ? (" motion "+std::to_string(event.motion.xrel)+","+std::to_string(event.motion.yrel)).c_str() : "");
                    }
                    pad_last_used=false;
                }
                if(false) {}
                else if(event.type==SDL_EVENT_KEY_DOWN || event.type==SDL_EVENT_MOUSE_BUTTON_DOWN || event.type==SDL_EVENT_MOUSE_MOTION || event.type==SDL_EVENT_MOUSE_WHEEL) {pad_last_used=false;wii_last_used=false;}
#ifndef _WIN32
                pad_last_used=true;   // Steam Deck: a touch is a mouse to SDL, but the prompts stay the Deck's buttons, never a mouse or a keyboard
#endif
                // Hold the speed key (` by default): game clock x8 (the harness's `speed` command), back to x1 on release or focus loss
                if(!network && event.type==SDL_EVENT_WINDOW_FOCUS_LOST) uc::set_clock_speed(1);
                if((event.type==SDL_EVENT_KEY_UP || event.type==SDL_EVENT_KEY_DOWN) && std::find(speed_keys.begin(),speed_keys.end(),event.key.scancode)!=speed_keys.end() && !settings_menu->capturing()) {
                    if(!network && !event.key.repeat) uc::set_clock_speed(event.type==SDL_EVENT_KEY_DOWN && !host_keys.menu_open ? 8 : 1);
                    continue;
                }
                if(event.type==SDL_EVENT_KEY_DOWN && !event.key.repeat && host_key_pressed(event.key.scancode)) continue;
                if(event.type==SDL_EVENT_KEY_UP && (uc::is_menu_key(event.key.scancode,menu_keys) || event.key.scancode==SDL_SCANCODE_P) && !settings_menu->capturing()) continue;   // the releases of host keys
                if(deck_menu && host_keys.menu_open && deck_menu->event(event)) continue;   // the pad, the sticks and the keys of the Deck menu; a touch reaches it as the mouse below
                if(host_keys.menu_open) {
                    if(event.type==SDL_EVENT_WINDOW_FOCUS_LOST) settings_menu->focus_lost();
                    if(settings_menu->capturing()) settings_menu->capture_event(event); else renderer.menu_event(event);
                    continue;
                }
                if(host_keys.paused) continue;   // nothing reaches the game while paused
                if(coop.swallows_mouse(event)) continue;   // a pointer is captured (Windows): every mouse reaches the game through the pointer backend instead
                int width=0,height=0;
                if(!SDL_GetWindowSizeInPixels(window.get(),&width,&height))throw std::runtime_error(SDL_GetError());
                host_input.extent(uint32_t(std::max(0,width)),uint32_t(std::max(0,height)));
                host_input.density(SDL_GetWindowPixelDensity(window.get()));
                if(host_input.event(event,input,pointer_captured())) {
                    capture_pointer(true);
                    input.focus(true);
                    input2.focus(true);
                }
                if(host_input.take_recentre()) SDL_WarpMouseInWindow(window.get(),float(width)/2/SDL_GetWindowPixelDensity(window.get()),float(height)/2/SDL_GetWindowPixelDensity(window.get()));   // the trackpad's cursor reached the window's edge: back to the middle, the aim goes on from where it is
            }
            if (frame_log) frame_events_ns+=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-events_start).count());
            {
                int width=0,height=0;
                if(SDL_GetWindowSizeInPixels(window.get(),&width,&height)) input.extent(uint32_t(std::max(0,width)),uint32_t(std::max(0,height)));   // stick speed and gyro counts are in pixels
                host_input.update(input,SDL_GetTicksNS());
                if(gamepad) gamepad->update(input,pad_allowed(),&input2);
                coop.update_remotes(pad_allowed(),options.wiimote);
                if(wiimote) {
                    if(wiimote->activity()!=wii_activity) {wii_activity=wiimote->activity();if(!coop.second_is_remote(0)) {wii_last_used=true;pad_last_used=false;}}   // the prompts follow the first player's remote
                    if(wiimote2 && wiimote2->activity()!=wii_activity2) {wii_activity2=wiimote2->activity();if(!coop.second_is_remote(1)) {wii_last_used=true;pad_last_used=false;}}
                    const unsigned wii_changed=wiimote->changes()+(wiimote2 ? wiimote2->changes() : 0);
                    if(wii_changed!=wii_changes || wii_last_used!=shown_wii_last) {wii_changes=wii_changed;shown_wii_last=wii_last_used;refresh_prompt_device();}   // a remote came or went, or the other device was used
                }
                if(gamepad && (gamepad->changes()!=pad_changes || pad_last_used!=shown_pad_last)) {pad_changes=gamepad->changes();shown_pad_last=pad_last_used;refresh_prompt_device();}   // a pad came or went, or the other device was used
                {   // the game's motors run the device in use, and only while the game gets input (a menu, a pause or another window silences them)
                    const uint64_t now_ms=SDL_GetTicks();
                    const bool remote_connected=(wiimote && wiimote->status().connected) || (wiimote2 && wiimote2->status().connected);
                    const auto device=wii_last_used && remote_connected ? uc::RumbleDevice::remote : pad_last_used ? uc::RumbleDevice::pad : uc::RumbleDevice::none;   // the device the player is using: a remote that is gone or a pad lying on the desk of a keyboard player get nothing
                    const auto second=coop.status();   // a second player: each player's motor goes to the kind of device that player has
                    const auto second_device=second.kind==uc::DeviceKind::gamepad ? uc::RumbleDevice::pad : second.kind==uc::DeviceKind::wiimote ? uc::RumbleDevice::remote : uc::RumbleDevice::none;
                    const auto rumble=second.joined ? rumble_router.step(uc::input_rumble_local(),pad_allowed(),device,second_device,now_ms)
                                                     : rumble_router.step(uc::input_rumble_local(),pad_allowed(),device,now_ms);   // input_rumble_local() every loop, allowed or not: it also takes the taps that came and went meanwhile
                    rumble_pad_starts+=rumble.pad_starts; rumble_wii_starts+=rumble.wii_starts;
                    if(now_ms-rumble_logged_ms>=2000) {   // diagnostics, only while something happens: the game's "motor on" commands against the taps the pad / the remote were given
                        const uint64_t starts=uc::input_rumble_starts();
                        if(starts!=rumble_game_starts || rumble_pad_starts || rumble_wii_starts)
                            std::fprintf(stderr,"[rumble] last 2 s: game started the motor %llu times, pad taps %u, remote taps %u (input allowed=%d, strength %u%%, pad=%d, remote in use=%d)\n",
                                         (unsigned long long)(starts-rumble_game_starts),rumble_pad_starts,rumble_wii_starts,int(pad_allowed()),options.rumble,int(gamepad && gamepad->status().connected),int(device==uc::RumbleDevice::remote));
                        rumble_game_starts=starts; rumble_pad_starts=rumble_wii_starts=0; rumble_logged_ms=now_ms;
                    }
                    if(gamepad) gamepad->rumble(rumble.pad,options.rumble/100.f);
                    if(wiimote) wiimote->rumble((rumble.wii&(coop.second_is_remote(0) ? 2u : 1u)) && options.rumble>0);   // the second player's remote takes bit 1, the first player's bit 0
                    if(wiimote2) wiimote2->rumble((rumble.wii&(coop.second_is_remote(1) ? 2u : 1u)) && options.rumble>0);
                }
            }
            if (watch_error) std::rethrow_exception(watch_error);
            service_frame();
            // A game frame is ~23 packets: do not pay a wait per packet. A pending renderer restart stops at the end of a frame.
            for (int drained = 0; drained < 64 && !close && !(restart_pending && at_frame_boundary) && render_pending(); ++drained) {}
            if (settings_menu) settings_menu->commit();   // the menu's picks, applied outside rendering
            if (deck_menu) deck_menu->commit();
            if (restart_pending && at_frame_boundary && !debug_capture_serial && !debug_capture_requested && !audit_capture_serial && !scene_capture_serial && !scene_dump_serial) {
                restart_pending=false;
                const auto began=std::chrono::steady_clock::now();
                if (!renderer.set_upscaling(options.upscaler,options.upscale_mode,options.sharpening,options.sharpness,options.frame_generation,options.frame_generation_multiplier)) {
                    restart_renderer();
                    apply_host_keys();   // the new renderer gets the menu back
                } else std::fprintf(stderr,"Upscaling updated in %.1f ms: %s, RCAS %s\n",
                    std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count(),
                    std::string(uc::upscaler_name(options.upscaler)).c_str(),options.sharpening ? "on" : "off");
                if (options.upscaler!=uc::UpscalerKind::off && renderer.active_upscaler()!=options.upscaler) settings_menu->upscaler_failed(options.upscaler);
                else if (options.frame_generation && !renderer.frame_generation_active()) settings_menu->frame_generation_failed(options.upscaler);
            }
        }
        auto now = std::chrono::steady_clock::now();
        if (options.seconds && now - start >= std::chrono::seconds(options.seconds)) close = true;
        if ((det_trace && det_trace->done()) || det_stop) close = true;
        if (now >= next_watch) {
            if ((options.gx_audit || options.profile || !uc::diagnostic_directory().empty()) && !options.headless) {
                const auto presents=renderer.presentation_count(),textures=renderer.texture_creations(),decodes=uc::gx_stats().texture_decodes;
                const auto profile_retrace=uc::vi_stats().retraces;
                const double profile_ms=std::chrono::duration<double,std::milli>(now-last_profile_time).count();
                std::fprintf(stderr,"[fps] window_ms %.3f retraces %llu:%llu presents %llu fps %.3f\n",profile_ms,(unsigned long long)last_profile_retrace,(unsigned long long)profile_retrace,(unsigned long long)(presents-last_profile_presents),1000.0*(presents-last_profile_presents)/profile_ms);
                last_profile_time=now; last_profile_retrace=profile_retrace;
                std::fprintf(stderr,"[gpu] passes: upscale %.2f ms, smaa %.2f ms, occlusion %.2f ms, shadow %.2f ms, motion blur %.2f ms (%llu runs), rt_as (last packet) %.2f ms, rt_shadows %.2f ms, bloom %.2f ms\n",renderer.pass_ms(uc::GpuPass::upscale),renderer.pass_ms(uc::GpuPass::smaa),renderer.pass_ms(uc::GpuPass::occlusion),renderer.pass_ms(uc::GpuPass::shadow),renderer.pass_ms(uc::GpuPass::motion_blur),(unsigned long long)renderer.pass_runs(uc::GpuPass::shadow),renderer.pass_ms(uc::GpuPass::rt_as),renderer.pass_ms(uc::GpuPass::rt_shadows),renderer.pass_ms(uc::GpuPass::bloom));
                std::fprintf(stderr,"[gpu-sum] per second: scene draws %.1f ms, efb draws %.1f ms, texture copies %.1f ms, bloom %.1f ms, upscale %.1f ms\n",renderer.take_pass_total_ms(uc::GpuPass::scene_draws),renderer.take_pass_total_ms(uc::GpuPass::efb_draws),renderer.take_pass_total_ms(uc::GpuPass::texture_copy),renderer.take_pass_total_ms(uc::GpuPass::bloom),renderer.take_pass_total_ms(uc::GpuPass::upscale));
                const auto copies=renderer.take_copy_stats();
                if(renderer.copy_stats_available()) std::fprintf(stderr,"[efb-copies] per second: %llu copies, %llu cover 90%% of the EFB, %llu clear it after, %.2f EFB areas copied\n",(unsigned long long)copies.count,(unsigned long long)copies.large,(unsigned long long)copies.clearing,double(copies.pixels)/(640.0*480.0));
                std::fprintf(stderr,"[perf] presents %llu render %.2f ms texture creates %llu CPU decodes %llu late-aim draws %llu cache evictions %llu cache MB %llu\n",(unsigned long long)(presents-last_profile_presents),render_ns/1e6,(unsigned long long)(textures-last_profile_textures),(unsigned long long)(decodes-last_profile_decodes),(unsigned long long)renderer.late_aim_draws(),(unsigned long long)(uc::gx_stats().texture_evictions-last_profile_evictions),(unsigned long long)(uc::gx_stats().texture_resident>>20));
                last_profile_presents=presents; last_profile_textures=textures; last_profile_decodes=decodes; last_profile_evictions=uc::gx_stats().texture_evictions; render_ns=0;
                if(!present_intervals.empty()) {
                    std::sort(present_intervals.begin(),present_intervals.end());
                    double sum=0; for(double v:present_intervals) sum+=v;
                    std::fprintf(stderr,"[frametime] n %zu avg %.2f p50 %.2f p99 %.2f max %.2f ms\n",present_intervals.size(),sum/present_intervals.size(),present_intervals[present_intervals.size()/2],present_intervals[std::min(present_intervals.size()-1,present_intervals.size()*99/100)],present_intervals.back());
                    present_intervals.clear();
                }
                const auto gfx=uc::gx_stats(); const auto sound=uc::audio_stats();
                std::fprintf(stderr,"[descriptor-pools] created %llu\n",(unsigned long long)renderer.descriptor_pool_creations());
                const auto pacing=uc::game_pacing_stats();
                std::fprintf(stderr,"[window] input focus %u\n",window ? unsigned((SDL_GetWindowFlags(window.get())&SDL_WINDOW_INPUT_FOCUS)!=0) : 0u);
                { const auto sw=sched.switch_stats(); static uint64_t last_switches=0,last_wake=0,last_run[32]={}; std::fprintf(stderr,"[sched] switches %llu avg wake %.1f us, run ms:",(unsigned long long)(sw.switches-last_switches),(sw.switches-last_switches) ? (sw.wake_ns-last_wake)/1e3/double(sw.switches-last_switches) : 0.0); last_switches=sw.switches; last_wake=sw.wake_ns; for(int id=1;id<32;++id) { const auto ns=sched.thread_run_ns(id); if(ns-last_run[id]) std::fprintf(stderr," #%d(%08X) %.0f",id,sched.thread_entry(id),(ns-last_run[id])/1e6); last_run[id]=ns; } std::fputc(10,stderr); }
                std::fprintf(stderr,"[sched-switches]%s\n",sched.switch_report().c_str());
                { const auto depth=packets.depth(); std::fprintf(stderr,"[packets] queued %zu outstanding %zu presenting in flight %zu\n",depth.queued,depth.outstanding,depth.frames_in_flight); }   // outstanding must stay near the guest lead (tens), never creep to kMaxOutstandingPackets
                std::fprintf(stderr,"[guest-blocked] renderer wait %.2f ms total (window %.2f, packets %llu),main loop waited %.2f ms this second, GPU busy %.2f ms total, frame latency avg %.2f max %.2f ms over %llu frames\n",gx::seal_wait_ns()/1e6,gx::seal_window_wait_ns()/1e6,(unsigned long long)gx::seal_count(),loop_wait_ns/1e6,renderer.gpu_busy_ns()/1e6,gx::frame_latency().total_ns/1e6/std::max<uint64_t>(gx::frame_latency().frames,1),gx::frame_latency().max_ns/1e6,(unsigned long long)gx::frame_latency().frames); loop_wait_ns=0;
                std::fprintf(stderr,"[game-pacing] calls %llu retraces %llu timestep %.6f sec threshold2 %llu adjusted %llu wait %.2f ms\n",(unsigned long long)pacing.calls,(unsigned long long)pacing.retraces,pacing.timestep_ns/1e9,(unsigned long long)pacing.threshold_two,(unsigned long long)pacing.adjusted,pacing.wait_ns/1e6);
                if(audio) { const auto output=audio->stats(); std::fprintf(stderr,"[audio-output] blocks %llu overruns %llu dry %llu refills %llu max queued bytes %d\n",(unsigned long long)output.blocks,(unsigned long long)output.overruns,(unsigned long long)output.dry_writes,(unsigned long long)audio->refills(),output.max_queued_bytes); }
                std::fprintf(stderr,"[geometry-cache] hits %llu misses %llu\n",(unsigned long long)gfx.geometry_hits,(unsigned long long)gfx.geometry_misses);
                std::fprintf(stderr,"[profile] loops %llu retries %llu native draws %llu copies %llu audio blocks %llu\n",(unsigned long long)profile_loops,(unsigned long long)profile_retries,(unsigned long long)(gfx.draws-last_profile_draws),(unsigned long long)(gfx.copies-last_profile_copies),(unsigned long long)(sound.frames-last_audio_frames));
                std::fprintf(stderr,"[hitches] this second %u worst %.1f ms, session lines %u not logged %llu | %s\n",hitches_this_second,worst_hitch_ms,hitch_lines,(unsigned long long)hitch_unlogged,uc::dvd_area().c_str());
                hitches_this_second=hitch_second_lines=0; worst_hitch_ms=0;
                uc::log_runtime_sample();
                { static uint64_t last_cycles=0,last_bench_presents=0; const auto cycles=sched.guest_cycles(); std::fprintf(stderr,"[bench] presents %llu draws %llu guest_mcycles %.2f\n",(unsigned long long)(presents-last_bench_presents),(unsigned long long)(gfx.draws-last_profile_draws),(cycles-last_cycles)/1e6); last_cycles=cycles; last_bench_presents=presents; }
                profile_loops=profile_retries=0; last_profile_draws=gfx.draws; last_profile_copies=gfx.copies; last_audio_frames=sound.frames;
            }
            next_watch = now + std::chrono::seconds(1);
            uint64_t retraces = uc::vi_stats().retraces;
            if(network && !network->error().empty()) {result=5;close=true;}
            else if(network && network->waiting()) {boot_seconds=stalled=0;last_retraces=retraces;}
            else if (!retraces) { if (++boot_seconds >= 15) { result = 4; close = true; } }
            else if (retraces != last_retraces) { last_retraces = retraces; stalled = 0; }
            else if (++stalled >= 10) { result = 4; close = true; }
            if (result==4) std::fprintf(stderr, "no progress: retrace count %llu\n", (unsigned long long)retraces);
        }
        service_overlay();
        if(!options.headless && !pending && (!packets.detached() || frame_limit.ready(true,std::chrono::steady_clock::now())) && packets.wait_ready(std::chrono::nanoseconds(0))) continue;
        const auto wait_start=std::chrono::steady_clock::now();
        // With the reticle layer waiting for its next refresh the loop wakes a little before it is due (precise_sleep_until does the rest).
        const auto wake_in=overlay_idle ? std::clamp<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(overlay_due-wait_start-std::chrono::microseconds(1300)).count(),0,1000000) : int64_t(1000000);
        if(options.headless) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        else if(!renderer.wait_for_pending(uint64_t(wake_in))) {
            if(!pending && (!packets.detached() || frame_limit.ready(true,std::chrono::steady_clock::now()))) packets.wait_ready(overlay_idle ? std::chrono::nanoseconds(wake_in) : std::chrono::nanoseconds(std::chrono::milliseconds(1)));
            else if(!overlay_idle) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        loop_wait_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-wait_start).count();
    }
    network_wait_screen.reset();
    runner.stop();
    if(network && !network->error().empty()) {
        result=5;
        if(!controlled)SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Network play",network->error().c_str(),window.get());
    }
    if(test_pipes) { if(!test_released) test_server->release_all(); test_pipes->send("{\"type\":\"exit\",\"code\":"+std::to_string(result)+"}",false); }
    if (!overlay_intervals.empty()) {
        auto sorted=overlay_intervals; std::sort(sorted.begin(),sorted.end()); double sum=0; for(double v:sorted) sum+=v;
        size_t late=0,very_late=0; for(double v:sorted) { late+=v>6.5; very_late+=v>10.5; }
        std::fprintf(stderr,"[reticle-layer] intervals over 6.5 ms %zu, over 10.5 ms %zu of %zu\n",late,very_late,sorted.size());
        std::fprintf(stderr,"[reticle-layer] idle presents %zu interval ms mean %.2f p50 %.2f p95 %.2f p99 %.2f max %.2f\n",sorted.size(),sum/sorted.size(),sorted[sorted.size()/2],sorted[sorted.size()*95/100],sorted[sorted.size()*99/100],sorted.back());
    }
    const auto prompt_stats=uc::gx_stats();
    std::fprintf(stderr,"native prompt stats: annotations %llu present %llu fallback %llu\n",(unsigned long long)prompt_stats.prompt_annotations,(unsigned long long)prompt_stats.prompt_present,(unsigned long long)prompt_stats.prompt_fallback);
    uc::input_hle_set_probe(nullptr);
    const auto probe_stats=uc::input_probe_stats();
    if(options.input_probe) std::fprintf(stderr,"input probe stats: samples %llu changes %llu records %llu\n",(unsigned long long)probe_stats.samples,(unsigned long long)probe_stats.changes,(unsigned long long)probe_stats.records);
    const auto sound_stats=uc::audio_stats();
    const auto event_stats=uc::entry_probe_stats();
    const auto picture_stats=uc::picture_probe_stats();
    std::fprintf(stderr,"picture probe stats: calls %llu records %llu invalid %llu\n",(unsigned long long)picture_stats.calls,(unsigned long long)picture_stats.records,(unsigned long long)picture_stats.invalid);
    std::fprintf(stderr,"player event probe stats: events %llu records %llu invalid %llu\n",(unsigned long long)event_stats.events,(unsigned long long)event_stats.records,(unsigned long long)event_stats.invalid);
    if(frame_log) { char text[64]; std::string notes="# guest thread entry points:"; for(int t=1;t<=8;++t) {std::snprintf(text,sizeof text," %d=%08X",t,sched.thread_entry(t)); notes+=text;} std::snprintf(text,sizeof text,"\n# frame t_ms 0 = %.1f ms steady\n",double(std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch()).count())/1e6); notes+=text; frame_log->note(notes+sched.idle_report()); }
    frame_log.reset();   // joins the writer thread after the last rows
    std::fprintf(stderr,"audio stats: native frames %llu nonzero samples %llu\n",(unsigned long long)sound_stats.frames,(unsigned long long)sound_stats.nonzero_samples);
    if(options.audio_audit) {   // off in the public release (cleared at startup)
        fs::create_directories(exe_dir()/"debug");
        const auto path=exe_dir()/"debug"/("audio-gaps-"+std::to_string(process_id())+".txt");
        std::FILE* out=uc::fopen_path(path,"w");
        if(!out) throw std::runtime_error("cannot create audio audit report");
        uc::audio_gap_audit_report(out);
        const bool failed=std::ferror(out)!=0;
        const int closed=std::fclose(out);
        if(failed || closed!=0) throw std::runtime_error("cannot write audio audit report");
        std::fprintf(stderr,"audio gap report: %s\n",path.string().c_str());
    }
    uc::audio_hle_shutdown();
    uc::game_input_shutdown();
    uc::input_hle_init(nullptr);
    uc::save_hle_shutdown();
    uc::dvd_hle_shutdown();
    if (!options.headless) {
        std::fprintf(stderr, "presentation stats: queue presents %llu\n", (unsigned long long)renderer.presentation_count());
        renderer.shutdown();
    }
    if (options.validation && renderer.validation_errors()) result = 4;
    g_fifo_snapshot.store(g_fifo.bytes);
    std::fprintf(stderr, "runtime stopped; all guest and file workers joined\n");
    if (options.gx_gaps) {
        std::error_code error; fs::create_directories(exe_dir() / "debug", error);
        const auto path = exe_dir() / "debug" / ("gx-gaps-" + std::to_string(process_id()) + ".txt");
        if (std::FILE* out = uc::fopen_path(path, "w")) { uc::gx_gap_audit_report(out); std::fclose(out); std::fprintf(stderr, "gx gap report: %s\n", path.string().c_str()); }
    }
    uc::trace_dump_all(stderr, 24);
    print_stats();
    return result;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
namespace uc { void text_to_huge_pages(); }   // huge_text_posix.cpp
#if defined(__ANDROID__)
int uc_program_main(int argc, char** argv) {   // Android: called by the app's SDL_main (android_app_main.cpp) or by uc_cli over adb (android_cli_main.cpp)
#else
int main(int argc, char** argv) {   // Linux arguments are bytes already (UTF-8)
#endif
    uc::text_to_huge_pages();   // first, while this is the only thread
#endif
    uc::Options startup_options;
    std::optional<uc::DiagnosticLog> diagnostics;
    try {
#ifdef _WIN32
        auto update_session=uc::enter_client_session(exe_dir());
        if(auto* error=std::get_if<std::error_code>(&update_session)) {
            std::fprintf(stderr,"[update] Client is updating or requires recovery. Start uc_launcher.exe: %s\n",error->message().c_str());
            return 2;
        }
#endif
        if(!kPublicRelease)diagnostics.emplace("game",exe_dir()/"logs");
#ifdef _WIN32
        std::vector<std::string> encoded;
        std::vector<char*> arguments;
        encoded.reserve(size_t(argc));arguments.reserve(size_t(argc));
        for(int i=0;i<argc;++i) {
            const int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,nullptr,0,nullptr,nullptr);
            if(!size)throw std::invalid_argument("invalid Unicode argument");
            std::string text(size_t(size),'\0');
            if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,text.data(),size,nullptr,nullptr))throw std::runtime_error("cannot encode Unicode argument");
            text.pop_back();encoded.push_back(std::move(text));
        }
        for(auto& text:encoded)arguments.push_back(text.data());
        const int result=real_main(argc, arguments.data(),startup_options);
#else
        const int result=real_main(argc, argv,startup_options);
#endif
        std::fprintf(stderr,"[session] game exit=%d\n",result);
        return result;
    } catch (const std::exception& e) {
        const auto logs=kPublicRelease ? fs::path() : uc::diagnostic_directory().empty() ? exe_dir()/"logs" : uc::diagnostic_directory();
        std::fprintf(stderr,"[startup] %s\n",e.what());
        if(const auto message=uc::startup_error_message(startup_options,e.what(),logs)) {
            if(!SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Umbrella Chronicles",message->c_str(),nullptr))
                std::fprintf(stderr,"[startup] cannot show error dialog: %s\n",SDL_GetError());
        }
        diagnostics.reset(); // fatal exits without unwinding; drain the diagnostic pipe first.
        uc::fatal("startup failed: %s", e.what());
    }
}
