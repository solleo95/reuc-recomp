#include "launch.h"
#include "options_config.h"
#include "client_update_lock.h"
#include <fstream>
#include <iterator>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
namespace uc {
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
std::string query_features(const std::filesystem::path& executable,const std::filesystem::path& config,unsigned timeout_ms,std::optional<RendererBackend> backend) {
    if(backend && renderer_backend_name(*backend).empty())throw std::invalid_argument("invalid feature-query renderer");
    const auto exe=std::filesystem::absolute(executable);
    if(!std::filesystem::is_regular_file(exe))return {};
    const auto output=std::filesystem::temp_directory_path()/("uc-features-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())+".txt");
    SECURITY_ATTRIBUTES inherit{sizeof(inherit),nullptr,TRUE};
    HANDLE out=CreateFileW(output.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&inherit,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(out==INVALID_HANDLE_VALUE)return {};
    HANDLE null=CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_WRITE,&inherit,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    auto command=quote_windows_arg(exe.wstring())+L" --features --config "+quote_windows_arg(std::filesystem::absolute(config).wstring());
    if(backend) {const auto name=renderer_backend_name(*backend);command+=L" --renderer "+std::wstring(name.begin(),name.end());}
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdOutput=out;startup.hStdError=null==INVALID_HANDLE_VALUE ? out : null;startup.hStdInput=nullptr;
    PROCESS_INFORMATION process{};
    std::string text;
    if(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,exe.parent_path().c_str(),&startup,&process)) {
        std::fprintf(stderr,"[launch] GPU probe child pid=%lu timeout_ms=%u\n",process.dwProcessId,timeout_ms);
        CloseHandle(process.hThread);
        // The report is read as soon as it is written: the child's exit (GPU driver and NGX teardown) takes 8 s or more and is not waited for.
        const auto deadline=GetTickCount64()+timeout_ms;bool reported=false,exited=false;
        while(!exited && !reported && GetTickCount64()<deadline) {
            exited=WaitForSingleObject(process.hProcess,100)==WAIT_OBJECT_0;
            std::ifstream in(output,std::ios::binary);text.assign(std::istreambuf_iterator<char>(in),{});
            reported=!last_json_line(text).empty();
        }
        if(!exited) {
            if(!reported)std::fprintf(stderr,"[launch] GPU probe timed out\n");
            TerminateProcess(process.hProcess,reported ? 0 : 1);if(!reported)WaitForSingleObject(process.hProcess,5000);
        }
        DWORD code=0;GetExitCodeProcess(process.hProcess,&code);std::fprintf(stderr,"[launch] GPU probe exit=%lu\n",code);
        CloseHandle(process.hProcess);
    }
    CloseHandle(out);if(null!=INVALID_HANDLE_VALUE)CloseHandle(null);
    {std::ifstream in(output,std::ios::binary);text.assign(std::istreambuf_iterator<char>(in),{});}
    std::error_code ignored;std::filesystem::remove(output,ignored);
    return last_json_line(text);
}
GameProcess launch_game(const std::filesystem::path& executable,const std::filesystem::path& config,const Options* diagnostic,int guest_cpu,std::optional<net::Endpoint> network) {
    if(guest_cpu < -1 || guest_cpu>=63)throw std::invalid_argument("guest CPU must be -1 or 0..62");
    const auto exe=std::filesystem::absolute(executable),settings=std::filesystem::absolute(config);
    auto session=enter_client_session(exe.parent_path());
    if(auto* error=std::get_if<std::error_code>(&session)) throw std::runtime_error("Client update blocks game launch: "+error->message());
    if(!std::filesystem::is_regular_file(exe))throw std::runtime_error("uc.exe is missing");
    if(!std::filesystem::is_regular_file(settings))throw std::runtime_error("config.json is missing");
    const auto options=options_from_config(load_config(settings));
    if(!std::filesystem::is_regular_file(options.game/"DATA"/"sys"/"main.dol"))throw std::runtime_error("game folder does not contain DATA/sys/main.dol");
    auto command=quote_windows_arg(exe.wstring())+L" --config "+quote_windows_arg(settings.wstring());
    if(network) {
        if(!network->port || (!network->host && !net::valid_address(network->address)))throw std::invalid_argument("invalid network endpoint");
        command+=network->host ? L" --net-host" : L" --net-join "+quote_windows_arg(std::wstring(network->address.begin(),network->address.end()));
        command+=L" --net-port "+std::to_wstring(network->port);
        if(network->loopback)command+=L" --net-loopback";
    }
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
    std::fprintf(stderr,"[launch] game child pid=%lu network=%s port=%u\n",process.dwProcessId,network ? (network->host ? "host" : "client") : "off",network ? network->port : 0);
    CloseHandle(process.hThread);return GameProcess(process.hProcess);
}
}
