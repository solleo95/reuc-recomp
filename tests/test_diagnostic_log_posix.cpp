// Linux twin of test_diagnostic_log: stdout and stderr are captured into <session>/runtime.log while the inherited outputs still receive everything.
#ifndef __linux__
int main() { return 0; }
#else
#include "check.h"
#include "diagnostic_log.h"
#include "test_pid.h"
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

static std::string read(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};
}
int main() {
    std::filesystem::path name(u8"uc-logs-тест "); name+=std::to_string(test_process_id());   // spaces and non-ASCII letters in the log folder
    const auto root=std::filesystem::temp_directory_path()/name;
    std::filesystem::create_directories(root);
    const int original_out=dup(1),original_err=dup(2);
    CHECK(original_out>=0 && original_err>=0);
    CHECK(std::freopen((root/"parent-out.txt").c_str(),"wb",stdout) && std::freopen((root/"parent-err.txt").c_str(),"wb",stderr));
    std::filesystem::path session;
    {
        uc::DiagnosticLog log("test",root);
        CHECK(log.active());session=uc::diagnostic_directory();
        std::fprintf(stdout,"stdout preserved\n");
        std::fprintf(stderr,"stderr preserved\n");
        const char native[]="native output preserved\n";
        CHECK(write(1,native,sizeof(native)-1)==ssize_t(sizeof(native)-1));
        std::vector<std::thread> writers;
        for(int thread=0;thread<4;++thread)writers.emplace_back([thread] {
            for(int line=0;line<100;++line)std::fprintf(stderr,"worker-%d-line-%d\n",thread,line);
        });
        for(auto& writer:writers)writer.join();
    }
    CHECK(uc::diagnostic_directory().empty());
    std::fprintf(stdout,"after shutdown\n");std::fflush(stdout);std::fflush(stderr);
    CHECK(dup2(original_out,1)==1 && dup2(original_err,2)==2);
    close(original_out);close(original_err);
    const auto text=read(session/"runtime.log");
    CHECK(text.find("stdout preserved")!=std::string::npos && text.find("stderr preserved")!=std::string::npos);
    CHECK(text.find("native output preserved")!=std::string::npos && text.find("log closed normally")!=std::string::npos);
    CHECK(text.find("after shutdown")==std::string::npos);
    for(int thread=0;thread<4;++thread)for(int line=0;line<100;++line)
        CHECK(text.find("worker-"+std::to_string(thread)+"-line-"+std::to_string(line)+"\n")!=std::string::npos);
    CHECK(read(root/"parent-out.txt").find("native output preserved")!=std::string::npos);
    CHECK(read(root/"parent-out.txt").find("after shutdown")!=std::string::npos);
    CHECK(read(root/"parent-err.txt").find("stderr preserved")!=std::string::npos);
    {std::ofstream blocked(root/"not-directory");blocked<<"fixture";}
    {uc::DiagnosticLog log("test",root/"not-directory");CHECK(!log.active());}
    CHECK(uc::diagnostic_directory().empty());
    std::filesystem::remove_all(root);
}
#endif
