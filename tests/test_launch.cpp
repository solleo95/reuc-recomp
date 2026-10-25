#include "check.h"
#include "launch.h"
#include "options_config.h"
#include <fstream>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
int main(int argc,char** argv) {
    if(argc>2 && std::string_view(argv[1])=="--config") {
        std::ofstream out(std::filesystem::path(argv[2]).parent_path()/"child-args.txt");
        for(int i=1;i<argc;++i)out<<argv[i]<<'\n';
        out<<"console="<<(GetConsoleWindow()!=nullptr)<<'\n';
        return out ? 0 : 3;
    }
    for(auto value:{L"",L"a b",L"тест",L"a\"b",L"a\\",L"x\\\"y",L"C:\\A B\\"}) {
        auto command=L"program.exe "+uc::quote_windows_arg(value);int count=0;
        auto arguments=CommandLineToArgvW(command.c_str(),&count);
        CHECK(arguments && count==2 && std::wstring(arguments[1])==value);LocalFree(arguments);
    }
    bool failed=false;try {uc::launch_game(L"Z:/missing/uc.exe",L"Z:/missing/config.json");}catch(const std::exception&){failed=true;}CHECK(failed);
    failed=false;try {uc::quote_windows_arg(std::wstring_view(L"a\0b",3));}catch(const std::invalid_argument&){failed=true;}CHECK(failed);
    CHECK(uc::last_json_line("File 'sl.dlss.dll' is signed by NVIDIA\r\n{\"a\":1}\r\n{\"dlss\":true}\r\n")=="{\"dlss\":true}");   // the report, not Streamline's chatter
    CHECK(uc::last_json_line("no report here\n").empty() && uc::last_json_line("").empty() && uc::last_json_line("{unterminated").empty());
    CHECK(uc::query_features(L"Z:/missing/uc.exe",L"Z:/missing/config.json").empty());
    const auto directory=std::filesystem::temp_directory_path()/("uc-launch-test-"+std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory/"DATA/sys");
    {std::ofstream dol(directory/"DATA/sys/main.dol");dol<<"fixture";}
    uc::ConfigDocument config;uc::Options options;options.game=directory;
    uc::store_options(config,options);uc::save_config(directory/"config.json",config);
    wchar_t executable[32768];CHECK(GetModuleFileNameW(nullptr,executable,32768));
    for(int mode:{0,1,2}) {
        const int cpu=mode==1 ? 7 : -1;
        std::optional<uc::net::Endpoint> network;
        if(mode)network=uc::net::Endpoint{mode==1,"127.0.0.1",27889,mode==1};
        auto process=uc::launch_game(executable,directory/"config.json",nullptr,cpu,network);
        CHECK(WaitForSingleObject(process.get(),10000)==WAIT_OBJECT_0);
        DWORD code=1;CHECK(GetExitCodeProcess(process.get(),&code) && code==0);
        std::ifstream in(directory/"child-args.txt");std::string args(std::istreambuf_iterator<char>(in),{});
        CHECK(args.find("console=0\n")!=std::string::npos);
        CHECK((args.find("--guest-cpu\n7\n")!=std::string::npos)==(cpu==7));
        if(cpu==-1)CHECK(args.find("--guest-cpu")==std::string::npos);
        if(mode) {
            CHECK(args.find("--net-port\n27889\n")!=std::string::npos);
            CHECK((args.find("--net-loopback\n")!=std::string::npos)==network->loopback);
            CHECK(args.find(cpu==7 ? "--net-host\n" : "--net-join\n127.0.0.1\n")!=std::string::npos);
        } else CHECK(args.find("--net-")==std::string::npos);
    }
    for(int cpu:{-2,63,999}) {
        failed=false;try {uc::launch_game(executable,directory/"config.json",nullptr,cpu);}catch(const std::invalid_argument&){failed=true;}CHECK(failed);
    }
    std::filesystem::remove_all(directory);
    return 0;
}
