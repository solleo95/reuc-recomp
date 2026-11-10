#include "fatal.h"
#include "crash_report.h"
#include "guest_mem.h"
#include "trace.h"
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#if defined(__APPLE__)
#include <sys/ucontext.h>   // the signal context only (<ucontext.h> is the deprecated makecontext API)
#else
#include <ucontext.h>
#endif
#include <unistd.h>

namespace uc {
static void (*g_hook)();
void set_exit_hook(void (*fn)()) { g_hook = fn; }

[[noreturn]] static void die(int code) {
    if (g_hook) g_hook();
    std::fflush(stderr);
#ifdef __ANDROID__
    usleep(300000);   // stderr reaches logcat and the session log through threads (game/android_app_main.cpp): let them take the last lines
#endif
    _exit(code);   // kills the other host threads too
}

// The crash path calls stdio and malloc inside a signal handler, which is not async-signal-safe; it runs once, right before the process ends, and
// SA_RESETHAND makes a second fault inside it terminate the process instead of looping.
static void on_signal(int signal_number, siginfo_t* info, void* context) {
    const auto* registers = static_cast<const ucontext_t*>(context);
#if defined(__APPLE__) && defined(__aarch64__)
    const void* pc = reinterpret_cast<void*>(__darwin_arm_thread_state64_get_pc(registers->uc_mcontext->__ss));
#elif defined(__aarch64__)   // Linux on arm64 (Android)
    const void* pc = reinterpret_cast<void*>(registers->uc_mcontext.pc);
#else
    const void* pc = reinterpret_cast<void*>(registers->uc_mcontext.gregs[REG_RIP]);
#endif
    const char* name = signal_number == SIGSEGV ? "SIGSEGV" : signal_number == SIGBUS ? "SIGBUS" : signal_number == SIGILL ? "SIGILL"
                     : signal_number == SIGFPE ? "SIGFPE" : signal_number == SIGABRT ? "SIGABRT" : "signal";
    const bool address_fault = signal_number == SIGSEGV || signal_number == SIGBUS;
    char reason[256];
    std::snprintf(reason, sizeof reason, "host %s (%d) at %p%s %p", name, signal_number, pc, address_fault ? " accessing" : "", address_fault ? info->si_addr : nullptr);
    crash_report_write(reason, nullptr);
    std::fprintf(stderr, "%s\n", reason);
    if (address_fault && guest_mem_base()) {
        const uint64_t offset = reinterpret_cast<uint64_t>(info->si_addr) - reinterpret_cast<uint64_t>(guest_mem_base());
        if (offset < 0x40000000u) std::fprintf(stderr, " = guest %08X", 0x80000000u | uint32_t(offset));
    }
    std::fprintf(stderr, "\nlast guest functions on this thread:\n");
    trace_dump(stderr);
    die(139);
}
static void terminate_handler() {
    try {
        if (auto e = std::current_exception()) std::rethrow_exception(e);
        std::fprintf(stderr, "terminate called without an exception\n");
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "terminate: uncaught C++ exception: %s\n", ex.what());
        char reason[512];  // no heap allocation: this path also runs for bad_alloc
        std::snprintf(reason, sizeof reason, "uncaught C++ exception: %s", ex.what());
        crash_report_write(reason, nullptr);
    } catch (...) {
        crash_report_write("uncaught non-standard exception", nullptr);
        std::fprintf(stderr, "terminate: uncaught non-standard exception\n");
    }
    trace_dump(stderr);
    die(3);
}
void install_crash_handlers() {
    crash_guard_thread();
    struct sigaction action{};
    action.sa_sigaction = on_signal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    for (const int signal_number : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(signal_number, &action, nullptr);
    std::set_terminate(terminate_handler);
}

void fatal(const char* fmt, ...) {
    std::va_list ap; va_start(ap, fmt);
    char message[1024];
    std::vsnprintf(message, sizeof message, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "fatal: %s\n", message);
    char reason[1100];
    std::snprintf(reason, sizeof reason, "fatal: %s", message);
    crash_report_write(reason, nullptr);
    trace_dump(stderr);
    die(3);
}
void fatal_guest(uint32_t addr, const char* what) {
    std::fprintf(stderr, "guest trap at %08X: %s", addr, what);
    std::string s = symbol_name(addr);
    if (!s.empty()) std::fprintf(stderr, "  [%s]", s.c_str());
    char reason[512];
    std::snprintf(reason, sizeof reason, "guest trap at %08X: %s%s%s", addr, what, s.empty() ? "" : " ", s.c_str());
    crash_report_write(reason, nullptr);
    std::fprintf(stderr, "\nlast guest functions on this thread:\n");
    trace_dump(stderr);
    die(3);
}
}  // namespace uc
