#include "diagnostic_log.h"
#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <string>
#include <io.h>
#include <fcntl.h>
#include <intrin.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>

namespace uc {
namespace {
std::filesystem::path directory; // Published before workers start; unchanged until they join.
std::atomic<unsigned> sequence{0};
bool valid(HANDLE handle) {return handle && handle!=INVALID_HANDLE_VALUE;}
void close(HANDLE& handle) {if(valid(handle))CloseHandle(handle);handle=nullptr;}
bool write(HANDLE handle,const char* data,size_t size) {
    while(size) {
        DWORD written=0;
        if(!WriteFile(handle,data,DWORD(size),&written,nullptr) || !written)return false;
        data+=written;size-=written;
    }
    return true;
}
std::string utf8(std::wstring_view text) {
    const int size=WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);
    std::string result(size,'\0');
    if(size)WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),result.data(),size,nullptr,nullptr);
    return result;
}
}
struct DiagnosticLog::Impl {
    HANDLE file=nullptr;
    std::mutex mutex;
    ULONGLONG started=GetTickCount64();
    bool file_failed=false;
    struct Pipe {
        Impl& owner;
        FILE* stream;
        DWORD standard;
        const char* name;
        HANDLE read=nullptr,mirror=nullptr;
        int saved=-1;
        bool redirected=false;
        std::thread worker;
        Pipe(Impl& owner,FILE* stream,DWORD standard,const char* name):owner(owner),stream(stream),standard(standard),name(name) {}
        void start() {
            const auto original=GetStdHandle(standard);
            if(valid(original) && !DuplicateHandle(GetCurrentProcess(),original,GetCurrentProcess(),&mirror,0,FALSE,DUPLICATE_SAME_ACCESS))
                throw std::runtime_error("cannot preserve diagnostic output");
            if(_fileno(stream)<0) {
                FILE* reopened=nullptr;
                if(freopen_s(&reopened,"NUL","w",stream))throw std::runtime_error("cannot initialize diagnostic stream");
            }
            std::fflush(stream);
            saved=_dup(_fileno(stream));
            if(saved<0)throw std::runtime_error("cannot preserve diagnostic stream");
            HANDLE writer=nullptr;
            if(!CreatePipe(&read,&writer,nullptr,65536))throw std::runtime_error("cannot create diagnostic pipe");
            const int fd=_open_osfhandle(intptr_t(writer),_O_WRONLY|_O_BINARY);
            if(fd<0) {close(writer);throw std::runtime_error("cannot open diagnostic pipe");}
            const int result=_dup2(fd,_fileno(stream));_close(fd);
            if(result)throw std::runtime_error("cannot redirect diagnostic stream");
            redirected=true;
            std::setvbuf(stream,nullptr,_IONBF,0);
            if(!SetStdHandle(standard,HANDLE(_get_osfhandle(_fileno(stream)))))throw std::runtime_error("cannot redirect native diagnostic output");
            worker=std::thread([this] {
                char bytes[8192];DWORD count=0;
                std::string pending;
                while(ReadFile(read,bytes,sizeof(bytes),&count,nullptr) && count) {
                    pending.append(bytes,count);
                    size_t first=0;
                    for(size_t end=pending.find('\n');end!=std::string::npos;end=pending.find('\n',first)) {
                        owner.append(name,pending.data()+first,end-first+1,mirror);first=end+1;
                    }
                    pending.erase(0,first);
                    if(pending.size()>=65536) {owner.append(name,pending.data(),pending.size(),mirror);pending.clear();}
                    if(valid(mirror) && !write(mirror,bytes,count))close(mirror); // A closed harness/console must not stop disk logging.
                }
                if(!pending.empty())owner.append(name,pending.data(),pending.size(),mirror);
            });
        }
        void stop() noexcept {
            if(redirected) {
                std::fflush(stream);
                if(_dup2(saved,_fileno(stream))!=0) {
                    _close(_fileno(stream)); // Release the pipe writer even if restoring the inherited handle fails.
                    SetStdHandle(standard,nullptr);
                } else SetStdHandle(standard,HANDLE(_get_osfhandle(_fileno(stream))));
                redirected=false;
            }
            if(saved>=0) {_close(saved);saved=-1;}
            if(worker.joinable())worker.join();
            close(read);close(mirror);
        }
        ~Pipe(){stop();}
    } out{*this,stdout,STD_OUTPUT_HANDLE,"stdout"},err{*this,stderr,STD_ERROR_HANDLE,"stderr"};
    void append(const char* source,const char* text,size_t size,HANDLE mirror) {
        std::lock_guard lock(mutex);
        if(file_failed)return;
        char prefix[80];const int count=std::snprintf(prefix,sizeof(prefix),"[+%llu ms %s] ",GetTickCount64()-started,source);
        if(!write(file,prefix,size_t(count)) || !write(file,text,size) || (text[size-1]!='\n' && !write(file,"\n",1))) {
            file_failed=true;
            constexpr char warning[]="diagnostic log write failed; output remains available on inherited handles\n";
            if(valid(mirror))write(mirror,warning,sizeof(warning)-1);
        }
    }
    ~Impl() {
        err.stop();out.stop();
        if(valid(file))FlushFileBuffers(file);
        close(file);
    }
};
DiagnosticLog::DiagnosticLog(std::string_view app,const std::filesystem::path& root) noexcept {
    try {
        if(!directory.empty())throw std::runtime_error("diagnostics already active");
        SYSTEMTIME time;GetSystemTime(&time);
        char stamp[96];std::snprintf(stamp,sizeof(stamp),"-%04u%02u%02u-%02u%02u%02u-%03u-%lu-%u",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,time.wMilliseconds,GetCurrentProcessId(),sequence.fetch_add(1));
        std::string label;
        for(char c:app)label+=((c>='a' && c<='z') || (c>='0' && c<='9') || c=='-') ? c : '_';
        const auto session=root/(label+stamp);
        std::filesystem::create_directories(session);
        auto impl=std::make_unique<Impl>();
        impl->file=CreateFileW((session/"runtime.log").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(!valid(impl->file))throw std::runtime_error("cannot create diagnostic log");
        impl->out.start();impl->err.start();
        directory=session;impl_=std::move(impl);
        SYSTEM_INFO system{};GetNativeSystemInfo(&system);
        MEMORYSTATUSEX memory{sizeof(memory)};GlobalMemoryStatusEx(&memory);
        RTL_OSVERSIONINFOW version{sizeof(version)};
        using Version=LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        if(auto query=reinterpret_cast<Version>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion")))query(&version);
        char brand[49]{};int cpu[4];__cpuid(cpu,0x80000000);
        if(unsigned(cpu[0])>=0x80000004)for(int i=0;i<3;++i) {__cpuid(cpu,0x80000002+i);std::memcpy(brand+i*16,cpu,16);}
        wchar_t executable[32768]{};const DWORD length=GetModuleFileNameW(nullptr,executable,32768);
        const auto path=utf8(std::wstring_view(executable,length));
        std::fprintf(stderr,"[session] app=%s pid=%lu UTC=%04u-%02u-%02uT%02u:%02u:%02uZ executable=%s\n",label.c_str(),GetCurrentProcessId(),time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,path.c_str());
        std::fprintf(stderr,"[system] Windows %lu.%lu build %lu CPU=%s logical=%lu RAM_total_MB=%llu RAM_free_MB=%llu build=%s %s\n",version.dwMajorVersion,version.dwMinorVersion,version.dwBuildNumber,brand,system.dwNumberOfProcessors,memory.ullTotalPhys/(1024*1024),memory.ullAvailPhys/(1024*1024),__DATE__,__TIME__);
    } catch(const std::exception& error) {std::fprintf(stderr,"[diagnostics] unavailable: %s\n",error.what());}
}
DiagnosticLog::~DiagnosticLog() {
    if(impl_) {std::fprintf(stderr,"[session] log closed normally\n");impl_.reset();directory.clear();}
}
bool DiagnosticLog::active() const noexcept {return bool(impl_);}
const std::filesystem::path& diagnostic_directory() noexcept {return directory;}
}
