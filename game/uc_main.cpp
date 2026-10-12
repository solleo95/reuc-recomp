#include "clock.h"
#include "dispatch.h"
#include "dol.h"
#include "dvdfs.h"
#include "fatal.h"
#include "game_decl.h"
#include "guest_mem.h"
#include "gx_audit.h"
#include "hle/os.h"
#include "hle/save.h"
#include "native_input.h"
#include "hle/gx.h"
#include "hle/os_layout.h"
#include "lowmem.h"
#include "mmio.h"
#include "sched.h"
#include "trace.h"
#include "vi.h"
#include "options.h"
#include "intro.h"
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
struct FifoSink : uc::MmioDevice {   // native resource/vertex ingestion; unowned state bytes remain TD-014
    uint64_t bytes = 0;
    uint32_t read(uint32_t, int) override { return 0; }
    void write(uint32_t, int size, uint32_t value) override { bytes += uint64_t(size); uc::gx_audit_fifo_write(size,value); uc::gx_fifo_write(size, value); }
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
            for (const auto& resource : entry.resources) {
                std::fprintf(stderr,"gx resource: address %08X size %zu captured %zu hex",resource.address,resource.size,resource.bytes.size());
                for (auto byte : resource.bytes) std::fprintf(stderr," %02X",std::to_integer<uint8_t>(byte));
                std::fprintf(stderr,"\n");
            }
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
    if (auto* error = std::get_if<std::string>(&parsed)) { std::fprintf(stderr, "%s\nusage: uc [--game <dir>] [--seconds N] [--headless] [--width N] [--height N] [--validation] [--log-dvd] [--log-sleep] [--gx-audit] [--skip-intro]\n", error->c_str()); return 2; }
    const auto options = std::get<uc::Options>(std::move(parsed));
    const auto& game = options.game;
    uc::intro_init(options.skip_intro);
    if (options.gx_audit) {
#ifdef UC_TRACE
        uc::gx_audit_enable();
#else
        std::fprintf(stderr, "--gx-audit requires a trace build (scripts\\bringup.bat)\n"); return 2;
#endif
    }
    uc::os_log_sleep(options.log_sleep);
    uc::install_crash_handlers();
    uc::set_exit_hook(print_stats);
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
    uc::SaveFs saves(exe_dir()/"saves");
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
    if (!options.headless) {
        if (options.width > INT32_MAX || options.height > INT32_MAX) throw std::invalid_argument("window extent exceeds integer limits");
        SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE,"0",SDL_HINT_OVERRIDE);
        SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_SPEED_SCALE,"1",SDL_HINT_OVERRIDE);
        video.emplace();
        window.reset(SDL_CreateWindow("Resident Evil: The Umbrella Chronicles", int(options.width), int(options.height), SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE));
        if (!window) throw std::runtime_error(SDL_GetError());
        if (!SDL_SetWindowRelativeMouseMode(window.get(),true)) throw std::runtime_error(SDL_GetError());
        input.focus((SDL_GetWindowFlags(window.get())&SDL_WINDOW_INPUT_FOCUS)!=0);
        renderer.initialize(window.get(), options.validation, exe_dir() / "data" / "shaders");
    }
    uc::input_hle_init(options.headless ? nullptr : &input);
    gx::Packets packets;
    uc::gx_hle_init(sched, boot.mem, options.headless ? nullptr : &packets);
    sched.set_shutdown_hook([&packets] { packets.cancel(); });
    GuestRunner runner(sched, mt, packets);
    auto start = std::chrono::steady_clock::now(), next_watch = start + std::chrono::seconds(1);
    uint64_t last_retraces = 0;
    uint64_t last_profile_presents=0,last_profile_textures=0,last_profile_decodes=0,render_ns=0;
    int stalled = 0, boot_seconds = 0, result = 0;
    bool close = false;
    std::unique_ptr<const gx::FramePacket> pending;
    uint64_t audit_capture_serial=0;
    bool audit_captured=false;
    bool audit_late_captured=false;
    bool audit_seen_geometry=false;
    while (!close && !runner.done.load()) {
        if (!options.headless) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) close = true;
                if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) renderer.resize(uint32_t(std::max(0, event.window.data1)), uint32_t(std::max(0, event.window.data2)));
                if (event.type==SDL_EVENT_WINDOW_FOCUS_LOST) input.focus(false);
                if (event.type==SDL_EVENT_WINDOW_FOCUS_GAINED) input.focus(SDL_GetWindowRelativeMouseMode(window.get()));
                if (event.type==SDL_EVENT_KEY_DOWN && event.key.key==SDLK_ESCAPE) {
                    if (!SDL_SetWindowRelativeMouseMode(window.get(),false)) throw std::runtime_error(SDL_GetError());
                    input.focus(false);
                }
                if (event.type==SDL_EVENT_MOUSE_MOTION && SDL_GetWindowRelativeMouseMode(window.get())) {
                    int width=0,height=0;
                    if (!SDL_GetWindowSizeInPixels(window.get(),&width,&height)) throw std::runtime_error(SDL_GetError());
                    input.move_raw(event.motion.xrel,event.motion.yrel,uint32_t(std::max(0,width)),uint32_t(std::max(0,height)));
                }
                if (event.type==SDL_EVENT_MOUSE_BUTTON_DOWN || event.type==SDL_EVENT_MOUSE_BUTTON_UP) {
                    const bool down=event.type==SDL_EVENT_MOUSE_BUTTON_DOWN;
                    if (down && event.button.button==SDL_BUTTON_LEFT && !SDL_GetWindowRelativeMouseMode(window.get())) {
                        if (!SDL_SetWindowRelativeMouseMode(window.get(),true)) throw std::runtime_error(SDL_GetError());
                        input.focus(true);
                    } else if (event.button.button==SDL_BUTTON_LEFT) input.button(uc::kButtonA,down);
                }
            }
            if (!pending) pending = packets.try_take();
            if (pending) {
                const auto render_start=std::chrono::steady_clock::now();
                const bool rendered=renderer.render(*pending);
                render_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-render_start).count();
                if (rendered) {
                audit_seen_geometry |= !pending->draws.empty();
                const bool late_frame=renderer.presentation_count()>=120 && std::chrono::steady_clock::now()-start>=std::chrono::seconds(15);
                if (options.gx_audit && !audit_late_captured && !audit_capture_serial && audit_seen_geometry && pending->present && (!audit_captured || late_frame)) audit_capture_serial=pending->serial;
                pending.reset();
                }
            }
            for (uint64_t serial : renderer.poll_completions()) {
                if (packets.complete(serial)) uc::gx_completed(serial);
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
            if (options.gx_audit && !options.headless) {
                const auto presents=renderer.presentation_count(),textures=renderer.texture_creations(),decodes=uc::gx_stats().texture_decodes;
                std::fprintf(stderr,"[perf] presents %llu render %.2f ms texture creates %llu CPU decodes %llu\n",(unsigned long long)(presents-last_profile_presents),render_ns/1e6,(unsigned long long)(textures-last_profile_textures),(unsigned long long)(decodes-last_profile_decodes));
                last_profile_presents=presents; last_profile_textures=textures; last_profile_decodes=decodes; render_ns=0;
            }
            next_watch = now + std::chrono::seconds(1);
            uint64_t retraces = uc::vi_stats().retraces;
            if (!retraces) { if (++boot_seconds >= 15) { result = 4; close = true; } }
            else if (retraces != last_retraces) { last_retraces = retraces; stalled = 0; }
            else if (++stalled >= 10) { result = 4; close = true; }
            if (result) std::fprintf(stderr, "no progress: retrace count %llu\n", (unsigned long long)retraces);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    runner.stop();
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
    uc::trace_dump_all(stderr, 24);
    print_stats();
    return result;
}

int main(int argc, char** argv) {
    try {
        return real_main(argc, argv);
    } catch (const std::exception& e) {
        uc::fatal("startup failed: %s", e.what());
    }
}
