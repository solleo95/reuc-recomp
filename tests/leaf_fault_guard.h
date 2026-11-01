#pragma once
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "check.h"
#include "ppc_rt.h"
#ifdef _WIN32
inline bool call_guarded(void (*fn)(ppc::Ctx&), ppc::Ctx& c) {
    __try { fn(c); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
#else
#include <atomic>
#include <csetjmp>
#include <csignal>
#include <cstdlib>

namespace leaf_fault {
static_assert(std::atomic<sigjmp_buf*>::is_always_lock_free);
inline thread_local std::atomic<sigjmp_buf*> checkpoint{nullptr};
inline void fault(int signal) {
    if (auto* saved=checkpoint.load(std::memory_order_relaxed)) siglongjmp(*saved,1);
    std::_Exit(128+signal);
}
}
// Generated leaves have trivial locals; random pointer chases must fault alike in both paths.
inline bool call_guarded(void (*fn)(ppc::Ctx&), ppc::Ctx& c) {
    struct sigaction action{},old_segv{},old_bus{};
    action.sa_handler=leaf_fault::fault;
    CHECK(sigemptyset(&action.sa_mask)==0);
    CHECK(sigaction(SIGSEGV,&action,&old_segv)==0);
    CHECK(sigaction(SIGBUS,&action,&old_bus)==0);
    sigjmp_buf saved;
    bool ok;
    if (sigsetjmp(saved,1)==0) {
        leaf_fault::checkpoint.store(&saved,std::memory_order_relaxed);
        fn(c);
        ok=true;
    } else ok=false;
    leaf_fault::checkpoint.store(nullptr,std::memory_order_relaxed);
    CHECK(sigaction(SIGBUS,&old_bus,nullptr)==0);
    CHECK(sigaction(SIGSEGV,&old_segv,nullptr)==0);
    return ok;
}
#endif
