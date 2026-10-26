#include "check.h"
#include "diagnostic_log.h"
#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

std::string read(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};
}
int main() {
    const auto root=std::filesystem::temp_directory_path()/(L"uc-logs-тест-"+std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    const int original_out=_dup(_fileno(stdout)),original_err=_dup(_fileno(stderr));
    CHECK(original_out>=0 && original_err>=0);
    FILE* replaced=nullptr;
    CHECK(!_wfreopen_s(&replaced,(root/L"parent-out.txt").c_str(),L"wb",stdout));
    CHECK(!_wfreopen_s(&replaced,(root/L"parent-err.txt").c_str(),L"wb",stderr));
    SetStdHandle(STD_OUTPUT_HANDLE,HANDLE(_get_osfhandle(_fileno(stdout))));
    SetStdHandle(STD_ERROR_HANDLE,HANDLE(_get_osfhandle(_fileno(stderr))));
    std::filesystem::path session;
    {
        uc::DiagnosticLog log("test",root);
        CHECK(log.active());session=uc::diagnostic_directory();
        std::fprintf(stdout,"stdout preserved\n");
        std::fprintf(stderr,"stderr preserved\n");
        const char native[]="native output preserved\n";DWORD count=0;
        CHECK(WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),native,sizeof(native)-1,&count,nullptr));
        std::vector<std::thread> writers;
        for(int thread=0;thread<4;++thread)writers.emplace_back([thread] {
            for(int line=0;line<100;++line)std::fprintf(stderr,"worker-%d-line-%d\n",thread,line);
        });
        for(auto& writer:writers)writer.join();
    }
    CHECK(uc::diagnostic_directory().empty());
    std::fprintf(stdout,"after shutdown\n");std::fflush(stdout);std::fflush(stderr);
    CHECK(!_dup2(original_out,_fileno(stdout)) && !_dup2(original_err,_fileno(stderr)));
    _close(original_out);_close(original_err);
    SetStdHandle(STD_OUTPUT_HANDLE,HANDLE(_get_osfhandle(_fileno(stdout))));
    SetStdHandle(STD_ERROR_HANDLE,HANDLE(_get_osfhandle(_fileno(stderr))));
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
