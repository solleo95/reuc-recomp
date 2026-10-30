#include "diagnostic_log.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>

// Linux counterpart of diagnostic_log_win.cpp: stdout and stderr go through pipes into <session>/runtime.log and are passed on to the inherited outputs.
namespace uc {
namespace {
std::filesystem::path directory; // Published before workers start; unchanged until they join.
std::atomic<unsigned> sequence{0};
bool write_all(int fd,const char* data,size_t size) {
    while(size) {
        const ssize_t written=::write(fd,data,size);
        if(written<0) {if(errno==EINTR)continue;return false;}
        data+=written;size-=size_t(written);
    }
    return true;
}
std::string first_value(const char* file,const char* key) {   // "key: value" or "KEY=value" line of a /proc or /etc text file
    std::ifstream in(file);std::string line;
    const size_t length=std::strlen(key);
    while(std::getline(in,line))
        if(line.compare(0,length,key)==0 && line.size()>length) {
            auto value=line.substr(length);
            while(!value.empty() && (value.front()==' ' || value.front()=='\t' || value.front()==':' || value.front()=='=' || value.front()=='"'))value.erase(0,1);
            while(!value.empty() && (value.back()=='"' || value.back()=='\r'))value.pop_back();
            return value;
        }
    return {};
}
}
struct DiagnosticLog::Impl {
    int file=-1;
    std::mutex mutex;
    std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
    bool file_failed=false;
    struct Pipe {
        Impl& owner;
        FILE* stream;
        int target;
        const char* name;
        int read_end=-1,mirror=-1;
        bool redirected=false;
        std::thread worker;
        Pipe(Impl& owner,FILE* stream,int target,const char* name):owner(owner),stream(stream),target(target),name(name) {}
        void start() {
            std::fflush(stream);
            mirror=dup(target); // -1 when the stream was closed: the log file alone then
            int ends[2];
            if(pipe2(ends,O_CLOEXEC))throw std::runtime_error("cannot create diagnostic pipe");
            if(dup2(ends[1],target)<0) {::close(ends[0]);::close(ends[1]);throw std::runtime_error("cannot redirect diagnostic stream");}
            ::close(ends[1]);read_end=ends[0];redirected=true;
            std::setvbuf(stream,nullptr,_IONBF,0);
            worker=std::thread([this] {
                char bytes[8192];
                std::string pending;
                for(;;) {
                    const ssize_t count=::read(read_end,bytes,sizeof(bytes));
                    if(count<0 && errno==EINTR)continue;
                    if(count<=0)break;
                    pending.append(bytes,size_t(count));
                    size_t first=0;
                    for(size_t end=pending.find('\n');end!=std::string::npos;end=pending.find('\n',first)) {
                        owner.append(name,pending.data()+first,end-first+1);first=end+1;
                    }
                    pending.erase(0,first);
                    if(pending.size()>=65536) {owner.append(name,pending.data(),pending.size());pending.clear();}
                    if(mirror>=0 && !write_all(mirror,bytes,size_t(count))) {::close(mirror);mirror=-1;} // A closed harness/console must not stop disk logging.
                }
                if(!pending.empty())owner.append(name,pending.data(),pending.size());
            });
        }
        void stop() noexcept {
            if(redirected) {
                std::fflush(stream);
                if(mirror<0 || dup2(mirror,target)<0)::close(target); // Restoring the inherited stream also releases the pipe's writer.
                redirected=false;
            }
            if(worker.joinable())worker.join();
            if(read_end>=0) {::close(read_end);read_end=-1;}
            if(mirror>=0) {::close(mirror);mirror=-1;}
        }
        ~Pipe(){stop();}
    } out{*this,stdout,STDOUT_FILENO,"stdout"},err{*this,stderr,STDERR_FILENO,"stderr"};
    void append(const char* source,const char* text,size_t size) {
        std::lock_guard lock(mutex);
        if(file_failed)return;
        const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
        char prefix[80];const int count=std::snprintf(prefix,sizeof(prefix),"[+%lld ms %s] ",static_cast<long long>(elapsed),source);
        if(!write_all(file,prefix,size_t(count)) || !write_all(file,text,size) || (text[size-1]!='\n' && !write_all(file,"\n",1))) {
            file_failed=true;
            constexpr char warning[]="diagnostic log write failed; output remains available on inherited handles\n";
            if(err.mirror>=0)write_all(err.mirror,warning,sizeof(warning)-1);
        }
    }
    ~Impl() {
        err.stop();out.stop();
        if(file>=0) {fsync(file);::close(file);}
    }
};
DiagnosticLog::DiagnosticLog(std::string_view app,const std::filesystem::path& root) noexcept {
    try {
        if(!directory.empty())throw std::runtime_error("diagnostics already active");
        timespec now;clock_gettime(CLOCK_REALTIME,&now);
        std::tm time{};gmtime_r(&now.tv_sec,&time);
        char stamp[96];std::snprintf(stamp,sizeof(stamp),"-%04d%02d%02d-%02d%02d%02d-%03ld-%d-%u",time.tm_year+1900,time.tm_mon+1,time.tm_mday,time.tm_hour,time.tm_min,time.tm_sec,now.tv_nsec/1000000,int(getpid()),sequence++);
        std::string label;
        for(char c:app)label+=((c>='a' && c<='z') || (c>='0' && c<='9') || c=='-') ? c : '_';
        const auto session=root/(label+stamp);
        std::filesystem::create_directories(session);
        auto impl=std::make_unique<Impl>();
        impl->file=::open((session/"runtime.log").c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0644);
        if(impl->file<0)throw std::runtime_error("cannot create diagnostic log");
        impl->out.start();impl->err.start();
        directory=session;impl_=std::move(impl);
        utsname system{};uname(&system);
        const auto os=first_value("/etc/os-release","PRETTY_NAME");
        const auto cpu=first_value("/proc/cpuinfo","model name");
        const auto total=first_value("/proc/meminfo","MemTotal"),available=first_value("/proc/meminfo","MemAvailable");   // "<n> kB"
        char executable[4096]{};const ssize_t length=readlink("/proc/self/exe",executable,sizeof(executable)-1);
        std::fprintf(stderr,"[session] app=%s pid=%d UTC=%04d-%02d-%02dT%02d:%02d:%02dZ executable=%s\n",label.c_str(),int(getpid()),time.tm_year+1900,time.tm_mon+1,time.tm_mday,time.tm_hour,time.tm_min,time.tm_sec,length>0 ? executable : "?");
        std::fprintf(stderr,"[system] %s kernel %s CPU=%s logical=%ld RAM_total_MB=%lld RAM_free_MB=%lld build=%s %s\n",os.c_str(),system.release,cpu.c_str(),sysconf(_SC_NPROCESSORS_ONLN),
                     std::atoll(total.c_str())/1024,std::atoll(available.c_str())/1024,__DATE__,__TIME__);
    } catch(const std::exception& error) {std::fprintf(stderr,"[diagnostics] unavailable: %s\n",error.what());}
}
DiagnosticLog::~DiagnosticLog() {
    if(impl_) {std::fprintf(stderr,"[session] log closed normally\n");impl_.reset();directory.clear();}
}
bool DiagnosticLog::active() const noexcept {return bool(impl_);}
const std::filesystem::path& diagnostic_directory() noexcept {return directory;}
}
