#include "fatal.h"
#include "guest_mem.h"
#include "trace.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <exception>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace uc {
static void (*g_hook)();
void set_exit_hook(void (*fn)()) { g_hook = fn; }

static void die(int code) {
    if (g_hook) g_hook();
    std::fflush(stderr);
    ExitProcess(UINT(code));   // kills the other host threads too
}
static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    std::fprintf(stderr, "host exception %08lX at %p", r->ExceptionCode, r->ExceptionAddress);
    if ((r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) && r->NumberParameters > 1)
        std::fprintf(stderr, " (%s address %p)", r->ExceptionInformation[0] ? "write to" : "read of", reinterpret_cast<void*>(r->ExceptionInformation[1]));
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters > 1 && guest_mem_base()) {
        uint64_t off = r->ExceptionInformation[1] - reinterpret_cast<uint64_t>(guest_mem_base());
        if (off < 0x40000000u) std::fprintf(stderr, " = guest %08X", 0x80000000u | uint32_t(off));
    }
    std::fprintf(stderr, "\nlast guest functions on this thread:\n");
    trace_dump(stderr);
    die(3);
    return EXCEPTION_EXECUTE_HANDLER;
}
static void terminate_handler() {
    try {
        if (auto e = std::current_exception()) std::rethrow_exception(e);
        std::fprintf(stderr, "terminate called without an exception\n");
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "terminate: uncaught C++ exception: %s\n", ex.what());
    } catch (...) {
        std::fprintf(stderr, "terminate: uncaught non-standard exception\n");
    }
    trace_dump(stderr);
    die(3);
}
void install_crash_handlers() {
    SetUnhandledExceptionFilter(crash_filter);
    std::set_terminate(terminate_handler);
}

void fatal(const char* fmt, ...) {
    std::va_list ap; va_start(ap, fmt);
    std::fprintf(stderr, "fatal: ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
    trace_dump(stderr);
    die(3);
}
void fatal_guest(uint32_t addr, const char* what) {
    std::fprintf(stderr, "guest trap at %08X: %s", addr, what);
    std::string s = symbol_name(addr);
    if (!s.empty()) std::fprintf(stderr, "  [%s]", s.c_str());
    std::fprintf(stderr, "\nlast guest functions on this thread:\n");
    trace_dump(stderr);
    die(3);
}
}  // namespace uc
