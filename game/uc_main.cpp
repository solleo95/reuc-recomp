#include "clock.h"
#include "dispatch.h"
#include "dol.h"
#include "dvdfs.h"
#include "fatal.h"
#include "crash_report.h"
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
#include "intro.h"
#include "game_pacing.h"
#include "render_scale.h"
#include "window_mode.h"
#include "frame_limit.h"
#include "audio_output.h"
#include "ax_audio.h"
#include "audio_gap_audit.h"
#include "vulkan_renderer.h"
#include <algorithm>
#include <atomic>
#include <chrono>
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
    std::vector<std::string_view> arguments;
    for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
    auto parsed = uc::parse_options(arguments);
    if (auto* error = std::get_if<std::string>(&parsed)) { std::fprintf(stderr, "%s\nusage: uc [--game <dir>] [--seconds N] [--headless] [--width N] [--height N] [--render-scale 0.5..6 (720p multiplier)] [--window-mode windowed|borderless|exclusive] [--unlock-60fps] [--validation] [--log-dvd] [--log-sleep] [--gx-audit] [--skip-intro] [--profile] [--input-probe] [--capture-scene-passes]\n", error->c_str()); return 2; }
    auto options = std::get<uc::Options>(std::move(parsed));
    const auto input_config=uc::load_config(options.config.empty() ? exe_dir()/"config.json" : options.config);
    parsed=uc::parse_options(arguments,uc::options_from_config(input_config));
    if(auto* error=std::get_if<std::string>(&parsed))throw std::invalid_argument(*error);
    options=std::get<uc::Options>(std::move(parsed));
    const bool controlled=!options.test_control.empty();
    if(controlled && (options.headless || options.save_root.empty())) throw std::invalid_argument("--test-control requires Vulkan and an isolated --save-root");
    if(controlled && options.window_mode!=uc::WindowMode::windowed) throw std::invalid_argument("process-local diagnostics require Windowed mode to preserve desktop focus");
    const auto& game = options.game;
    uc::intro_init(options.skip_intro);
    uc::game_pacing_init(options.unlock_60fps);
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
        return std::string(text);
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
        window.reset(SDL_CreateWindow("Resident Evil: The Umbrella Chronicles", int(options.width), int(options.height), SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | (controlled ? SDL_WINDOW_HIDDEN : 0) | (monitor_window && options.window_mode==uc::WindowMode::windowed ? SDL_WINDOW_MAXIMIZED : 0)));
        if (!window) throw std::runtime_error(SDL_GetError());
        uc::configure_window_mode(window.get(),options.window_mode,int(options.width),int(options.height));
        const char* mode_name=options.window_mode==uc::WindowMode::windowed ? "Windowed" : options.window_mode==uc::WindowMode::borderless ? "Borderless" : "Exclusive Fullscreen";
        std::fprintf(stderr,"Window mode: %s\n",mode_name);
        if(controlled) {
            const auto properties=SDL_GetWindowProperties(window.get());
            const auto hwnd=static_cast<HWND>(SDL_GetPointerProperty(properties,SDL_PROP_WINDOW_WIN32_HWND_POINTER,nullptr));
            if(!hwnd) throw std::runtime_error("diagnostic window handle unavailable");
            ShowWindow(hwnd,SW_SHOWNOACTIVATE);
        }
        if (!controlled && !SDL_SetWindowRelativeMouseMode(window.get(),true)) throw std::runtime_error(SDL_GetError());
        input.focus(controlled || (SDL_GetWindowFlags(window.get())&SDL_WINDOW_INPUT_FOCUS)!=0);
        renderer.initialize(window.get(), options.validation, exe_dir() / "data" / "shaders", options.render_scale,aspect);
        renderer.set_prompt_style(options.prompt_style);
    }
    uc::input_hle_init(options.headless ? nullptr : &input);
    uc::game_input_init(options.headless ? nullptr : &input,options.input_probe);
    uc::input_probe_init(options.input_probe);
    uc::entry_probe_init(options.input_probe);
    uc::prompt_hooks_init(input_config.input,options.input_probe);
    uc::input_hle_set_probe(options.input_probe ? uc::input_probe_sample : nullptr);
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
    uint64_t profile_loops=0,profile_retries=0,last_profile_draws=0,last_profile_copies=0,last_audio_frames=0;
    int stalled = 0, boot_seconds = 0, result = 0;
    bool close = false;
    std::unique_ptr<const gx::FramePacket> pending;
    uc::FrameLimit frame_limit;
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
            if(controlled && std::chrono::steady_clock::now()>=next_control) {
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
            if (!pending) pending = packets.try_take();
            // Keep the captured slot alive until its fence completes and readback finishes.
            if (pending && !debug_capture_serial && !audit_capture_serial && !scene_capture_serial && frame_limit.ready(pending->present,std::chrono::steady_clock::now())) {
                const auto render_start=std::chrono::steady_clock::now();
                const bool rendered=renderer.render(*pending);
                profile_retries+=!rendered;
                render_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-render_start).count();
                if (rendered) {
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
                        std::fprintf(stderr,"[scene-pass-last] callsite %08X vertices %zu stages %u blend %u:%u:%u depth %u\n",draw.callsite,draw.vertices.size(),draw.material.stages,draw.blend.mode,draw.blend.source,draw.blend.destination,draw.uniforms.pixel[1]);
                        for(uint32_t i=0;i<draw.material.stages;++i) {
                            const auto& stage=draw.material.stage[i];
                            std::fprintf(stderr,"[scene-pass-stage] %u color %06X alpha %06X order %03X konst %03X\n",i,stage.color,stage.alpha,stage.order,stage.konst);
                        }
                        for(uint32_t i=0;i<4;++i) {
                            const auto& r=draw.uniforms.registers[i]; const auto& k=draw.uniforms.konst[i];
                            std::fprintf(stderr,"[scene-pass-color] %u reg %.6g:%.6g:%.6g:%.6g konst %.6g:%.6g:%.6g:%.6g\n",i,r[0],r[1],r[2],r[3],k[0],k[1],k[2],k[3]);
                        }
                    }
                }
                if (pending->present) frame_limit.presented(std::chrono::steady_clock::now());
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
                    const auto directory=fs::path(L"S:\\Games\\Umbrella Chronicles\\debug");
                    const auto stamp=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                    const auto path=directory/("frame-"+std::to_string(stamp)+"-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(serial)+".png");
                    try {
                        fs::create_directories(directory);
                        uc::save_png(pixels,path);
                        std::fprintf(stderr,"debug screenshot: %s\n",path.string().c_str());
                    } catch(const std::exception& error) {
                        std::fprintf(stderr,"debug screenshot failed: %s\n",error.what());
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
        }
        auto now = std::chrono::steady_clock::now();
        if (options.seconds && now - start >= std::chrono::seconds(options.seconds)) close = true;
        if (now >= next_watch) {
            if ((options.gx_audit || options.profile) && !options.headless) {
                const auto presents=renderer.presentation_count(),textures=renderer.texture_creations(),decodes=uc::gx_stats().texture_decodes;
                std::fprintf(stderr,"[perf] presents %llu render %.2f ms texture creates %llu CPU decodes %llu\n",(unsigned long long)(presents-last_profile_presents),render_ns/1e6,(unsigned long long)(textures-last_profile_textures),(unsigned long long)(decodes-last_profile_decodes));
                last_profile_presents=presents; last_profile_textures=textures; last_profile_decodes=decodes; render_ns=0;
                const auto gfx=uc::gx_stats(); const auto sound=uc::audio_stats();
                std::fprintf(stderr,"[descriptor-pools] created %llu\n",(unsigned long long)renderer.descriptor_pool_creations());
                const auto pacing=uc::game_pacing_stats();
                std::fprintf(stderr,"[game-pacing] calls %llu retraces %llu timestep %.6f sec threshold2 %llu adjusted %llu\n",(unsigned long long)pacing.calls,(unsigned long long)pacing.retraces,pacing.timestep_ns/1e9,(unsigned long long)pacing.threshold_two,(unsigned long long)pacing.adjusted);
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
        if(options.headless) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        else if(!renderer.wait_for_pending(1000000)) {
            if(!pending) packets.wait_ready(std::chrono::milliseconds(1));
            else std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    runner.stop();
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
