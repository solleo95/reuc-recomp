#include "launch.h"
#include "options_config.h"
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
GameProcess launch_game(const std::filesystem::path& executable,const std::filesystem::path& config,const Options* diagnostic) {
    const auto exe=std::filesystem::absolute(executable),settings=std::filesystem::absolute(config);
    if(!std::filesystem::is_regular_file(exe))throw std::runtime_error("uc.exe is missing");
    if(!std::filesystem::is_regular_file(settings))throw std::runtime_error("config.json is missing");
    const auto options=options_from_config(load_config(settings));
    if(!std::filesystem::is_regular_file(options.game/"DATA"/"sys"/"main.dol"))throw std::runtime_error("game folder does not contain DATA/sys/main.dol");
    auto command=quote_windows_arg(exe.wstring())+L" --config "+quote_windows_arg(settings.wstring());
    if(diagnostic) {
        if(diagnostic->test_control.empty() || diagnostic->save_root.empty())throw std::invalid_argument("diagnostic launch requires isolated input/save paths");
        command+=L" --test-control "+quote_windows_arg(diagnostic->test_control.wstring())+L" --save-root "+quote_windows_arg(diagnostic->save_root.wstring());
        command+=L" --seconds 20 --skip-intro --window-mode windowed --width 1280 --height 720 --render-scale 1";
    }
    if(command.size()>=32767)throw std::invalid_argument("launch command exceeds Windows limit");
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,exe.parent_path().c_str(),&startup,&process))
        throw std::runtime_error("CreateProcessW failed: Windows error "+std::to_string(GetLastError()));
    CloseHandle(process.hThread);return GameProcess(process.hProcess);
}
}
