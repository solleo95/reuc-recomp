#include "launch.h"
#include "options_config.h"
#include <fstream>
#include <iterator>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
namespace uc {
std::wstring quote_windows_arg(std::wstring_view argument) {
    if(argument.find(L'\0')!=std::wstring_view::npos)throw std::invalid_argument("argument contains NUL");
    std::wstring result=L"\"";size_t slashes=0;
    for(wchar_t c:argument) {
        if(c==L'\\') {++slashes;continue;}
        result.append(c==L'\"' ? slashes*2+1 : slashes,L'\\');slashes=0;result+=c;
    }
    result.append(slashes*2,L'\\');result+=L'\"';return result;
}
void ProcessCloser::operator()(void* handle) const {if(handle)CloseHandle(handle);}
bool game_running(const GameProcess& process) {
    if(!process)return false;
    const auto result=WaitForSingleObject(process.get(),0);
    if(result==WAIT_FAILED)throw std::runtime_error("cannot inspect game process");
    return result==WAIT_TIMEOUT;
}
std::string last_json_line(std::string_view output) {
    std::string result;
    for(size_t begin=0;begin<output.size();) {
        auto end=output.find('\n',begin);if(end==std::string_view::npos)end=output.size();
        auto line=output.substr(begin,end-begin);begin=end+1;
        while(!line.empty() && (line.back()=='\r' || line.back()==' '))line.remove_suffix(1);
        if(line.size()>1 && line.front()=='{' && line.back()=='}')result=std::string(line);
    }
    return result;
}
std::string query_features(const std::filesystem::path& executable,const std::filesystem::path& config,unsigned timeout_ms) {
    const auto exe=std::filesystem::absolute(executable);
    if(!std::filesystem::is_regular_file(exe))return {};
    const auto output=std::filesystem::temp_directory_path()/("uc-features-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())+".txt");
    SECURITY_ATTRIBUTES inherit{sizeof(inherit),nullptr,TRUE};
    HANDLE out=CreateFileW(output.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&inherit,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(out==INVALID_HANDLE_VALUE)return {};
    HANDLE null=CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_WRITE,&inherit,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    auto command=quote_windows_arg(exe.wstring())+L" --features --config "+quote_windows_arg(std::filesystem::absolute(config).wstring());
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdOutput=out;startup.hStdError=null==INVALID_HANDLE_VALUE ? out : null;startup.hStdInput=nullptr;
    PROCESS_INFORMATION process{};
    std::string text;
    if(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,exe.parent_path().c_str(),&startup,&process)) {
        CloseHandle(process.hThread);
        if(WaitForSingleObject(process.hProcess,timeout_ms)==WAIT_TIMEOUT) {TerminateProcess(process.hProcess,1);WaitForSingleObject(process.hProcess,5000);}
        CloseHandle(process.hProcess);
    }
    CloseHandle(out);if(null!=INVALID_HANDLE_VALUE)CloseHandle(null);
    {std::ifstream in(output,std::ios::binary);text.assign(std::istreambuf_iterator<char>(in),{});}
    std::error_code ignored;std::filesystem::remove(output,ignored);
    return last_json_line(text);
}
GameProcess launch_game(const std::filesystem::path& executable,const std::filesystem::path& config,const Options* diagnostic,int guest_cpu) {
    if(guest_cpu < -1 || guest_cpu>=63)throw std::invalid_argument("guest CPU must be -1 or 0..62");
    const auto exe=std::filesystem::absolute(executable),settings=std::filesystem::absolute(config);
    if(!std::filesystem::is_regular_file(exe))throw std::runtime_error("uc.exe is missing");
    if(!std::filesystem::is_regular_file(settings))throw std::runtime_error("config.json is missing");
    const auto options=options_from_config(load_config(settings));
    if(!std::filesystem::is_regular_file(options.game/"DATA"/"sys"/"main.dol"))throw std::runtime_error("game folder does not contain DATA/sys/main.dol");
    auto command=quote_windows_arg(exe.wstring())+L" --config "+quote_windows_arg(settings.wstring());
    if(guest_cpu>=0)command+=L" --guest-cpu "+std::to_wstring(guest_cpu);
    if(diagnostic) {
        if(diagnostic->test_control.empty() || diagnostic->save_root.empty())throw std::invalid_argument("diagnostic launch requires isolated input/save paths");
        command+=L" --test-control "+quote_windows_arg(diagnostic->test_control.wstring())+L" --save-root "+quote_windows_arg(diagnostic->save_root.wstring());
        command+=L" --seconds 20 --skip-intro --window-mode windowed --width 1280 --height 720 --render-scale 1 --fps-limit 0 --mute";
    }
    if(command.size()>=32767)throw std::invalid_argument("launch command exceeds Windows limit");
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,exe.parent_path().c_str(),&startup,&process))
        throw std::runtime_error("CreateProcessW failed: Windows error "+std::to_string(GetLastError()));
    CloseHandle(process.hThread);return GameProcess(process.hProcess);
}
}
