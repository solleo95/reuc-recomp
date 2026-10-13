// Standalone Windows sampler for one already-running native game process.
// Build with clang-cl /O2 /EHsc cpu_sample.cpp; output addresses use module RVAs.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <map>
#include <string_view>
#include <thread>
#include <vector>

struct Handle {
    HANDLE value;
    ~Handle() { if (value && value!=INVALID_HANDLE_VALUE) CloseHandle(value); }
};
static uint64_t ticks(FILETIME t) { return uint64_t(t.dwHighDateTime)<<32 | t.dwLowDateTime; }
static uint64_t cpu(HANDLE h) {
    FILETIME created{},exited{},kernel{},user{};
    if (!GetThreadTimes(h,&created,&exited,&kernel,&user)) return 0;
    return ticks(kernel)+ticks(user);
}
static bool number(const char* text,uint32_t& out) {
    const std::string_view value(text);
    auto result=std::from_chars(value.data(),value.data()+value.size(),out);
    return result.ec==std::errc{} && result.ptr==value.data()+value.size();
}
int main(int argc,char** argv) {
    uint32_t pid=0,seconds=0;
    if(argc!=3 || !number(argv[1],pid) || !number(argv[2],seconds) || !pid || pid==GetCurrentProcessId() || !seconds || seconds>30) {
        std::fprintf(stderr,"usage: cpu_sample <other process PID> <seconds 1..30>\n"); return 2;
    }
    Handle modules{CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,pid)};
    MODULEENTRY32W module{}; module.dwSize=sizeof(module);
    if(!Module32FirstW(modules.value,&module)) { std::fprintf(stderr,"cannot enumerate process: %lu\n",GetLastError()); return 1; }
    const uint64_t base=reinterpret_cast<uintptr_t>(module.modBaseAddr),end=base+module.modBaseSize;
    std::printf("process %u image %ls base %llX\n",pid,module.szModule,static_cast<unsigned long long>(base));
    struct Thread { DWORD id; HANDLE handle; uint64_t before; std::map<uint64_t,uint32_t> samples; };
    std::vector<Thread> threads;
    Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0)};
    THREADENTRY32 entry{}; entry.dwSize=sizeof(entry);
    if(!Thread32First(snapshot.value,&entry)) return 1;
    do {
        if(entry.th32OwnerProcessID!=pid) continue;
        HANDLE h=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ThreadID);
        if(h) threads.push_back({entry.th32ThreadID,h,cpu(h),{}});
    } while(Thread32Next(snapshot.value,&entry));
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
    unsigned failures=0;
    while(std::chrono::steady_clock::now()<until) {
        for(auto& t:threads) {
            CONTEXT context{}; context.ContextFlags=CONTEXT_CONTROL;
            if(SuspendThread(t.handle)==DWORD(-1)) { ++failures; continue; }
            const bool captured=GetThreadContext(t.handle,&context)!=FALSE;
            // Resume before allocation, logging or touching another thread.
            if(ResumeThread(t.handle)==DWORD(-1)) {
                std::fprintf(stderr,"resume failed for thread %lu: %lu\n",t.id,GetLastError()); ++failures;
            }
            if(captured) ++t.samples[context.Rip]; else ++failures;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    for(auto& t:threads) {
        const auto after=cpu(t.handle);
        const auto elapsed=after>=t.before ? after-t.before : 0;
        if(elapsed>=100000) {
            std::printf("thread %lu CPU %.3f ms\n",t.id,double(elapsed)/10000);
            std::vector<std::pair<uint64_t,uint32_t>> sorted(t.samples.begin(),t.samples.end());
            std::sort(sorted.begin(),sorted.end(),[](auto a,auto b){return a.second>b.second;});
            for(size_t i=0;i<sorted.size();++i) {
                const auto [ip,count]=sorted[i];
                if(i>=12 && (ip<base || ip>=end)) continue;
                std::printf("  %u IP %llX",count,static_cast<unsigned long long>(ip));
                if(ip>=base && ip<end) std::printf(" RVA %llX",static_cast<unsigned long long>(ip-base));
                std::printf("\n");
            }
        }
        CloseHandle(t.handle);
    }
    std::printf("sampling failures %u\n",failures);
    return failures ? 1 : 0;
}
