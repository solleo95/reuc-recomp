#include "clock.h"
#include "compile_screen.h"
#include "dispatch.h"
#include "dol.h"
#include "dvdfs.h"
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
#include "hle/gx.h"
#include "hle/os_layout.h"
#include "lowmem.h"
#include "mmio.h"
#include "sched.h"
#include "trace.h"
#include "vi.h"
#include "options.h"
#include "test_control.h"
#include "test_server.h"
#include "test_state.h"
#include "test_draws.h"
#include "intro.h"
#include "game_pacing.h"
#include "render_scale.h"
#include "window_mode.h"
#include "frame_limit.h"
#include "cpu_sampler.h"
#include "audio_output.h"
#include "ax_audio.h"
#include "audio_gap_audit.h"
#include "vulkan_renderer.h"
#include <algorithm>
#include <atomic>
#include <chrono>
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
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>

extern const uc::FnEntry uc_functions[];
extern const size_t uc_function_count;
extern const uint32_t uc_text_begin, uc_text_end;
namespace fs = std::filesystem;

namespace {
uint32_t guest_writer() { // first translated function on the stack: the guest code storing to the FIFO
    void* frames[32];
    const auto count = RtlCaptureStackBackTrace(1, 32, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) if (const auto guest = uc::guest_function_for_host(frames[i])) return guest;
    return 0;
}
struct FifoSink : uc::MmioDevice {   // native resource/vertex ingestion; unowned state bytes remain TD-014
    uint64_t bytes = 0;
    uint32_t read(uint32_t, int) override { return 0; }
    void write(uint32_t, int size, uint32_t value) override { bytes += uint64_t(size); uc::gx_audit_fifo_write(size,value);
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
}

std::atomic<uint64_t> g_fifo_snapshot{0};

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

fs::path exe_dir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}
std::vector<uint8_t> slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
struct VideoSession {
    VideoSession() { if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError()); }
    ~VideoSession() { SDL_Quit(); }
};
struct NativeInputSink : uc::TestInputSink {
    uc::NativeInput& input; SDL_Window* window; const uint8_t* guest;
    NativeInputSink(uc::NativeInput& i, SDL_Window* w, const uint8_t* g) : input(i), window(w), guest(g) {}
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
        return input.apply_test_event(sequence, event, uint32_t(std::max(0, width)), uint32_t(std::max(0, height)));
    }
    uint64_t consumed() override { return input.last_consumed_sequence(); }
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

static int real_main(int argc, char** argv) {
    timeBeginPeriod(1);   // 1 ms timers: sleeps and the modal drag/resize loop's WM_TIMER tick (otherwise 15.6 ms granularity)
    {   // Windows 11 may run a window without focus in EcoQoS and ignore the timer request above; a game wants neither.
        PROCESS_POWER_THROTTLING_STATE throttling{PROCESS_POWER_THROTTLING_CURRENT_VERSION,PROCESS_POWER_THROTTLING_EXECUTION_SPEED|PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION,0};
        if(!SetProcessInformation(GetCurrentProcess(),ProcessPowerThrottling,&throttling,sizeof throttling)) std::fprintf(stderr,"power throttling opt-out failed: %lu\n",GetLastError());
    }
    std::vector<std::string_view> arguments;
    for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
    auto parsed = uc::parse_options(arguments);
    if (auto* error = std::get_if<std::string>(&parsed)) { std::fprintf(stderr, "%s\nusage: uc [--game <dir>] [--seconds N] [--headless] [--width N] [--height N] [--render-scale 0.5..6 (720p multiplier)] [--window-mode windowed|borderless|exclusive] [--unlock-60fps] [--game-hz 61..480] [--validation] [--log-dvd] [--log-sleep] [--gx-audit] [--skip-intro] [--no-vulkan-layers] [--profile] [--input-probe] [--capture-scene-passes] [--test-server <name> [--speed 1..8] [--capture-dir <dir>] [--god] [--skip-tutorials]]\n", error->c_str()); return 2; }
    auto options = std::get<uc::Options>(std::move(parsed));
    const auto input_config=uc::load_config(options.config.empty() ? exe_dir()/"config.json" : options.config);
    parsed=uc::parse_options(arguments,uc::options_from_config(input_config));
    if(auto* error=std::get_if<std::string>(&parsed))throw std::invalid_argument(*error);
    options=std::get<uc::Options>(std::move(parsed));
    const bool server_mode=!options.test_server.empty();
    if(server_mode && !options.test_control.empty()) throw std::invalid_argument("--test-server replaces --test-control; use only one");
    const bool controlled=!options.test_control.empty() || server_mode;
    if(controlled && (options.headless || options.save_root.empty())) throw std::invalid_argument("--test-control and --test-server require Vulkan and an isolated --save-root");
    if(controlled && options.window_mode!=uc::WindowMode::windowed) throw std::invalid_argument("process-local diagnostics require Windowed mode to preserve desktop focus");
    const auto& game = options.game;
    if(options.no_vulkan_layers && !SetEnvironmentVariableA("VK_LOADER_LAYERS_DISABLE","~implicit~")) throw std::runtime_error("cannot disable implicit Vulkan layers");   // read by the loader at vkCreateInstance
    uc::intro_init(options.skip_intro);
    uc::game_pacing_init(options.unlock_60fps,options.game_hz);
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
    uc::crash_report_init(exe_dir() / "debug", [] {
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
    uc::DvdFs dvd;
    if (!dvd.open_root(game / "DATA" / "files")) uc::fatal("game files not found under %s", (game / "DATA" / "files").string().c_str());
    uc::Sched sched(boot.mem);
    uc::SaveFs saves(options.save_root.empty() ? exe_dir()/"saves" : options.save_root);
    uc::save_hle_init(saves);
    uc::DvdSession dvd_session(dvd);
    uc::dvd_log(options.log_dvd);
    uc::set_clock_speed(options.speed);
    uc::test_state_enable(server_mode);
    uc::test_state_set_god(options.god);
    uc::test_state_set_skip_tutorials(options.skip_tutorials);
    uc::os_hle_init(sched, boot.mem, 0x817F0000);
    sched.irq_ctx().r[1] = 0x817EFF00;
    uc::vi_hle_init(sched, uc::kViPeriodNtsc, [] { g_fifo_snapshot.store(g_fifo.bytes, std::memory_order_relaxed); });

    // The start code runs on the SDK's DefaultThread: __OSThreadInit (802611B8) stores this OSThread in the current-thread word and fills it in.
    const uint32_t kMainThread = 0x805F88D8;
    uint32_t entry = dol.image.entry;
    uc::Thread* mt = sched.create(kMainThread, 16, [entry](ppc::Ctx& c) {
        c.r[1] = 0x817D0000;   // the start code installs its own stack pointer; this one only has to be valid
        uc::call_indirect(c, entry);
    }, &boot);

    std::optional<VideoSession> video;
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(nullptr, SDL_DestroyWindow);
    uc::VulkanRenderer renderer;
    uc::NativeInput input;
    input.bindings(input_config.input);
    uc::HostInput host_input;
    if (!options.headless) {
        if (options.width > INT32_MAX || options.height > INT32_MAX) throw std::invalid_argument("window extent exceeds integer limits");
        SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE,"0",SDL_HINT_OVERRIDE);
        SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_SPEED_SCALE,"1",SDL_HINT_OVERRIDE);
        video.emplace();
        const auto display=SDL_GetPrimaryDisplay();
        const auto* mode=display ? SDL_GetDesktopDisplayMode(display) : nullptr;
        if(!mode || mode->w<=0 || mode->h<=0) throw std::runtime_error("monitor display mode unavailable: "+std::string(SDL_GetError()));
        const double aspect=double(mode->w)/mode->h;
        const bool monitor_window=!options.width && !options.height;
        if(!options.width) options.width=uint32_t(mode->w);
        if(!options.height) options.height=uint32_t(mode->h);
        if(!options.render_scale) options.render_scale=uc::monitor_render_scale(uint32_t(mode->w),uint32_t(mode->h));
        std::fprintf(stderr,"Monitor: %dx%d %.3g Hz;720p multiplier %.4gx\n",mode->w,mode->h,mode->refresh_rate,options.render_scale);
        int window_width=int(options.width),window_height=int(options.height);
        if(monitor_window && options.window_mode==uc::WindowMode::windowed) {   // windowed start: the selected render resolution (shrunk only to fit the desktop)
            SDL_Rect usable{0,0,mode->w,mode->h};
            SDL_GetDisplayUsableBounds(display,&usable);
            const auto extent=uc::windowed_extent(options.render_scale,aspect,uint32_t(std::max(0,usable.w)),uint32_t(std::max(0,usable.h)));
            window_width=int(extent.width); window_height=int(extent.height);
        }
        window.reset(SDL_CreateWindow("Resident Evil: The Umbrella Chronicles", window_width, window_height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | (controlled ? SDL_WINDOW_HIDDEN : 0)));
        if (!window) throw std::runtime_error(SDL_GetError());
        uc::configure_window_mode(window.get(),options.window_mode,int(options.width),int(options.height));
        const char* mode_name=options.window_mode==uc::WindowMode::windowed ? "Windowed" : options.window_mode==uc::WindowMode::borderless ? "Borderless" : "Exclusive Fullscreen";
        std::fprintf(stderr,"Window mode: %s\n",mode_name);
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
        }
        if (!controlled && !SDL_SetWindowRelativeMouseMode(window.get(),true)) throw std::runtime_error(SDL_GetError());
        input.focus(controlled || (SDL_GetWindowFlags(window.get())&SDL_WINDOW_INPUT_FOCUS)!=0);
        renderer.initialize(window.get(), options.validation, exe_dir() / "data" / "shaders", options.render_scale,aspect);
        renderer.set_prompt_style(options.prompt_style);
        {   // compile every known pipeline now, so no pipeline is built while the game runs
            uc::CompileScreen screen;
            renderer.warm_pipelines([&screen](size_t done,size_t total) { screen.update(done,total); });
        }
    }
    uc::input_hle_init(options.headless ? nullptr : &input);
    uc::game_input_init(options.headless ? nullptr : &input,options.input_probe);
    uc::input_probe_init(options.input_probe);
    uc::entry_probe_init(options.input_probe);
    uc::prompt_hooks_init(input_config.input,options.input_probe);
    uc::input_hle_set_probe(options.input_probe ? uc::input_probe_sample : nullptr);
    std::optional<uc::TestPipes> test_pipes;
    std::optional<NativeInputSink> test_sink;
    std::optional<uc::TestServer> test_server;
    if (server_mode) { test_pipes.emplace(options.test_server); test_sink.emplace(input, window.get(), boot.mem); test_server.emplace(*test_pipes, *test_sink); }
    bool test_released = false, draws_wanted = false; uint32_t draws_pass = 0; std::vector<uc::TestDrawStat> draw_stats; std::string draw_projection;
    double last_render_ms = 0;
    std::optional<uc::AudioOutput> audio;
    if (!options.headless) audio.emplace();
    uc::audio_gap_audit_enable(options.audio_audit);
    uc::audio_hle_init(sched,[&audio](std::span<const int16_t> pcm) { if (audio) audio->write(pcm); },options.profile);
    gx::Packets packets;
    uc::gx_hle_init(sched, boot.mem, options.headless ? nullptr : &packets, options.profile);
    sched.set_shutdown_hook([&packets] { packets.cancel(); });
    GuestRunner runner(sched, mt, packets);
    auto start = std::chrono::steady_clock::now(), next_watch = start + std::chrono::seconds(1);
    uint64_t last_retraces = 0;
    uint64_t last_profile_presents=0,last_profile_textures=0,last_profile_decodes=0,render_ns=0;
    uint64_t loop_wait_ns=0,profile_loops=0,profile_retries=0,last_profile_draws=0,last_profile_copies=0,last_audio_frames=0;
    int stalled = 0, boot_seconds = 0, result = 0;
    bool close = false;
    std::unique_ptr<const gx::FramePacket> pending;
    uc::FrameLimit frame_limit;
    std::chrono::steady_clock::time_point last_present_at{};
    std::vector<double> present_intervals; present_intervals.reserve(1024);   // ms between presents, reported once per second by --profile
    uint64_t audit_capture_serial=0;
    uint64_t debug_capture_serial=0;
    uint64_t scene_capture_serial=0;
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
    auto render_pending = [&]() -> bool {
        bool progressed = false;
        if (!pending) pending = packets.try_take();
        // Keep the captured slot alive until its fence completes and readback finishes.
        if (pending && !debug_capture_serial && !audit_capture_serial && !scene_capture_serial && frame_limit.ready(pending->present,std::chrono::steady_clock::now())) {
            const auto render_start=std::chrono::steady_clock::now();
            const bool rendered=renderer.render(*pending);
            profile_retries+=!rendered;
            const auto render_elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-render_start).count();
            render_ns+=render_elapsed; last_render_ms=render_elapsed/1e6;
            if (rendered) {
            progressed = true;
            if(options.capture_scene_passes && !scene_capture_started) scene_capture_started=std::any_of(pending->draws.begin(),pending->draws.end(),[](const auto& draw){return draw.callsite==0x80366E14;});
            if(options.capture_scene_passes && scene_capture_started && scene_capture_pass<24) {
                scene_capture_serial=pending->serial; ++scene_capture_pass;
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
            if (pending->present) {
                const auto presented_at=std::chrono::steady_clock::now();
                if (last_present_at!=std::chrono::steady_clock::time_point{} && present_intervals.size()<present_intervals.capacity()) present_intervals.push_back(std::chrono::duration<double,std::milli>(presented_at-last_present_at).count());
                last_present_at=presented_at; frame_limit.presented(presented_at);
            }
            if (pending->present && debug_capture_requested) {
                debug_capture_serial=pending->serial;
                debug_capture_requested=false;
            }
            audit_seen_geometry |= !pending->draws.empty();
            const bool late_frame=renderer.presentation_count()>=120 && std::chrono::steady_clock::now()-start>=std::chrono::seconds(15);
            if (options.gx_audit && !audit_late_captured && !audit_capture_serial && audit_seen_geometry && pending->present && (!audit_captured || late_frame)) audit_capture_serial=pending->serial;
            pending.reset();
            }
        }
        for (uint64_t serial : renderer.poll_completions()) {
            if (packets.complete(serial)) uc::gx_completed(serial);
            if(serial==scene_capture_serial) {
                const auto pixels=renderer.capture(serial,true);
                const auto directory=fs::path(L"S:\\Games\\Umbrella Chronicles\\debug");
                const auto path=directory/("pass-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(scene_capture_pass)+"-"+std::to_string(serial)+".png");
                try {
                    fs::create_directories(directory); uc::save_png(pixels,path);
                    std::fprintf(stderr,"[scene-pass-capture] %s\n",path.string().c_str());
                } catch(const std::exception& error) { std::fprintf(stderr,"scene pass capture failed: %s\n",error.what()); }
                scene_capture_serial=0;
            }
            if (serial==debug_capture_serial) {
                const auto pixels=renderer.capture(serial);
                if (!pixels.width || !pixels.height || pixels.rgba.size()!=size_t(pixels.width)*pixels.height*4) throw std::runtime_error("invalid debug-frame readback");
                const auto directory=options.capture_dir.empty() ? fs::path(L"S:\\Games\\Umbrella Chronicles\\debug") : options.capture_dir;
                const auto stamp=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                const auto path=directory/("frame-"+std::to_string(stamp)+"-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(serial)+".png");
                try {
                    fs::create_directories(directory);
                    uc::save_png(pixels,path);
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
    // One loop iteration without event polling: harness commands, input services, rendering. Also run from the event watch,
    // so the harness (and the bot) keep working while Windows runs its modal drag/resize loop.
    auto service_frame = [&] {
        if(test_server) {
            const auto state=uc::test_state_latest();
            const auto pumped=test_server->pump(state.frame,uc::test_frame_json(state,uc::clock_speed(),last_render_ms,renderer.presentation_count(),uc::vi_stats().retraces,test_pipes->dropped()));
            debug_capture_requested|=pumped.capture; draws_wanted|=pumped.draws;
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
            if(!SDL_SetWindowRelativeMouseMode(window.get(),false))throw std::runtime_error(SDL_GetError());
            input.focus(false);
        }
        render_pending();
    };
    std::exception_ptr watch_error;
    bool in_render = false;
    struct Watch { std::function<void()>* render; std::function<void(uint32_t,uint32_t)>* resize; } watch_state;
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
    std::function<void(uint32_t,uint32_t)> resize_step = [&](uint32_t w, uint32_t h) { try { renderer.resize(w, h); } catch (...) { watch_error = std::current_exception(); } };
    watch_state = {&render_step, &resize_step};
    const auto watch = [](void* user, SDL_Event* event) -> bool {   // runs on the main thread, also inside the modal resize loop
        auto& state = *static_cast<Watch*>(user);
        if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) (*state.resize)(uint32_t(std::max(0, event->window.data1)), uint32_t(std::max(0, event->window.data2)));
        else if (event->type == SDL_EVENT_WINDOW_EXPOSED) (*state.render)();
        return true;
    };
    if (!options.headless) SDL_AddEventWatch(watch, &watch_state);
    uc::cpu_sampler_start_once("main");
    while (!close && !runner.done.load()) {
        ++profile_loops;
        if (!options.headless) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) close = true;
                if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) renderer.resize(uint32_t(std::max(0, event.window.data1)), uint32_t(std::max(0, event.window.data2)));
                if(controlled) continue;
                int width=0,height=0;
                if(!SDL_GetWindowSizeInPixels(window.get(),&width,&height))throw std::runtime_error(SDL_GetError());
                host_input.extent(uint32_t(std::max(0,width)),uint32_t(std::max(0,height)));
                if(host_input.event(event,input,SDL_GetWindowRelativeMouseMode(window.get()))) {
                    if(!SDL_SetWindowRelativeMouseMode(window.get(),true))throw std::runtime_error(SDL_GetError());
                    input.focus(true);
                }
            }
            if (watch_error) std::rethrow_exception(watch_error);
            service_frame();
            for (int drained = 0; drained < 64 && !close && render_pending(); ++drained) {}   // a game frame is ~23 packets: do not pay a wait per packet
        }
        auto now = std::chrono::steady_clock::now();
        if (options.seconds && now - start >= std::chrono::seconds(options.seconds)) close = true;
        if (now >= next_watch) {
            if ((options.gx_audit || options.profile) && !options.headless) {
                const auto presents=renderer.presentation_count(),textures=renderer.texture_creations(),decodes=uc::gx_stats().texture_decodes;
                std::fprintf(stderr,"[perf] presents %llu render %.2f ms texture creates %llu CPU decodes %llu\n",(unsigned long long)(presents-last_profile_presents),render_ns/1e6,(unsigned long long)(textures-last_profile_textures),(unsigned long long)(decodes-last_profile_decodes));
                last_profile_presents=presents; last_profile_textures=textures; last_profile_decodes=decodes; render_ns=0;
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
                { const auto depth=packets.depth(); std::fprintf(stderr,"[packets] queued %zu outstanding %zu presenting in flight %zu\n",depth.queued,depth.outstanding,depth.frames_in_flight); }   // outstanding must stay near the guest lead (tens), never creep to kMaxOutstandingPackets
                std::fprintf(stderr,"[guest-blocked] renderer wait %.2f ms total (window %.2f, packets %llu),main loop waited %.2f ms this second, GPU busy %.2f ms total, frame latency avg %.2f max %.2f ms over %llu frames\n",gx::seal_wait_ns()/1e6,gx::seal_window_wait_ns()/1e6,(unsigned long long)gx::seal_count(),loop_wait_ns/1e6,renderer.gpu_busy_ns()/1e6,gx::frame_latency().total_ns/1e6/std::max<uint64_t>(gx::frame_latency().frames,1),gx::frame_latency().max_ns/1e6,(unsigned long long)gx::frame_latency().frames); loop_wait_ns=0;
                std::fprintf(stderr,"[game-pacing] calls %llu retraces %llu timestep %.6f sec threshold2 %llu adjusted %llu wait %.2f ms\n",(unsigned long long)pacing.calls,(unsigned long long)pacing.retraces,pacing.timestep_ns/1e9,(unsigned long long)pacing.threshold_two,(unsigned long long)pacing.adjusted,pacing.wait_ns/1e6);
                if(audio) { const auto output=audio->stats(); std::fprintf(stderr,"[audio-output] blocks %llu overruns %llu max queued bytes %d\n",(unsigned long long)output.blocks,(unsigned long long)output.overruns,output.max_queued_bytes); }
                std::fprintf(stderr,"[geometry-cache] hits %llu misses %llu\n",(unsigned long long)gfx.geometry_hits,(unsigned long long)gfx.geometry_misses);
                std::fprintf(stderr,"[profile] loops %llu retries %llu native draws %llu copies %llu audio blocks %llu\n",(unsigned long long)profile_loops,(unsigned long long)profile_retries,(unsigned long long)(gfx.draws-last_profile_draws),(unsigned long long)(gfx.copies-last_profile_copies),(unsigned long long)(sound.frames-last_audio_frames));
                profile_loops=profile_retries=0; last_profile_draws=gfx.draws; last_profile_copies=gfx.copies; last_audio_frames=sound.frames;
            }
            next_watch = now + std::chrono::seconds(1);
            uint64_t retraces = uc::vi_stats().retraces;
            if (!retraces) { if (++boot_seconds >= 15) { result = 4; close = true; } }
            else if (retraces != last_retraces) { last_retraces = retraces; stalled = 0; }
            else if (++stalled >= 10) { result = 4; close = true; }
            if (result) std::fprintf(stderr, "no progress: retrace count %llu\n", (unsigned long long)retraces);
        }
        if(!options.headless && !pending && packets.wait_ready(std::chrono::nanoseconds(0))) continue;   // more work is queued
        const auto wait_start=std::chrono::steady_clock::now();
        if(options.headless) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        else if(!renderer.wait_for_pending(1000000)) {
            if(!pending) packets.wait_ready(std::chrono::milliseconds(1));
            else std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        loop_wait_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-wait_start).count();
    }
    runner.stop();
    if(test_pipes) { if(!test_released) test_server->release_all(); test_pipes->send("{\"type\":\"exit\",\"code\":"+std::to_string(result)+"}",false); }
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
    std::fprintf(stderr,"audio stats: native frames %llu nonzero samples %llu\n",(unsigned long long)sound_stats.frames,(unsigned long long)sound_stats.nonzero_samples);
    if(options.audio_audit) {
        fs::create_directories(exe_dir()/"debug");
        const auto path=exe_dir()/"debug"/("audio-gaps-"+std::to_string(GetCurrentProcessId())+".txt");
        std::FILE* out=_wfopen(path.c_str(),L"w");
        if(!out) throw std::runtime_error("cannot create audio audit report");
        uc::audio_gap_audit_report(out);
        const bool failed=std::ferror(out)!=0;
        const int closed=std::fclose(out);
        if(failed || closed!=0) throw std::runtime_error("cannot write audio audit report");
        std::fprintf(stderr,"audio gap report: %ls\n",path.c_str());
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
        const auto path = exe_dir() / "debug" / ("gx-gaps-" + std::to_string(GetCurrentProcessId()) + ".txt");
        if (std::FILE* out = _wfopen(path.c_str(), L"w")) { uc::gx_gap_audit_report(out); std::fclose(out); std::fprintf(stderr, "gx gap report: %ls\n", path.c_str()); }
    }
    uc::trace_dump_all(stderr, 24);
    print_stats();
    return result;
}

int wmain(int argc, wchar_t** argv) {
    try {
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
        return real_main(argc, arguments.data());
    } catch (const std::exception& e) {
        uc::fatal("startup failed: %s", e.what());
    }
}
