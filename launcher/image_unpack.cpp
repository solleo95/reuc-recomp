#include "image_unpack.h"
#include "windows_args.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <windows.h>
namespace fs=std::filesystem;
namespace uc::launcher {
namespace {
constexpr std::string_view expected_id="RBUE08";

uint64_t folder_size(const fs::path& folder) {
    uint64_t total=0;std::error_code error;
    for(fs::recursive_directory_iterator it(folder,fs::directory_options::skip_permission_denied,error),end;!error && it!=end;it.increment(error)) {
        std::error_code entry_error;
        if(it->is_regular_file(entry_error))total+=it->file_size(entry_error);
    }
    return total;
}
bool inside(const fs::path& file,const fs::path& folder) {
    std::error_code error;
    const auto real=fs::canonical(file,error);   // resolves junctions and links that lead into the folder
    for(auto parent=(error ? file : real).parent_path();;parent=parent.parent_path()) {
        if(fs::equivalent(parent,folder,error))return true;
        if(parent==parent.parent_path())return false;
    }
}
// The environment of this process plus LC_ALL: with a UTF-8 locale the Cygwin build of Wit opens paths with Cyrillic and other non-ASCII letters.
std::wstring environment_with_locale() {
    std::wstring block;
    if(wchar_t* strings=GetEnvironmentStringsW()) {
        for(const wchar_t* item=strings;*item;item+=std::wcslen(item)+1)
            if(_wcsnicmp(item,L"LC_ALL=",7))block.append(item,std::wcslen(item)+1);
        FreeEnvironmentStringsW(strings);
    }
    block+=L"LC_ALL=en_US.UTF-8";block.push_back(L'\0');block.push_back(L'\0');
    return block;
}
}
std::optional<int> parse_percent(std::string_view output) {
    constexpr std::string_view marker="% copied";   // not a bare '%': Wit echoes the image path too
    for(size_t sign=output.rfind(marker);sign!=std::string_view::npos;sign=sign ? output.rfind(marker,sign-1) : std::string_view::npos) {
        size_t begin=sign;while(begin && std::isdigit(static_cast<unsigned char>(output[begin-1])))--begin;
        if(begin<sign && sign-begin<=3)return std::stoi(std::string(output.substr(begin,sign-begin)));
    }
    return std::nullopt;
}
ImageUnpack::ImageUnpack(fs::path root,fs::path image,fs::path wit,uint64_t required_bytes)
    : root_(std::move(root)),image_(std::move(image)),wit_(wit.empty() ? root_/L"tools"/L"wit"/L"wit.exe" : std::move(wit)),required_bytes_(required_bytes) {
    worker_=std::jthread([this] {run();});
}
ImageUnpack::~ImageUnpack() {cancel();}
void ImageUnpack::cancel() {
    std::lock_guard lock(process_lock_);
    cancelled_=true;
    if(process_)TerminateProcess(process_,1);
}
bool ImageUnpack::stopped() {
    std::lock_guard lock(process_lock_);
    return cancelled_;
}
int ImageUnpack::wit(const std::wstring& arguments,std::string& output,bool track_progress) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit),nullptr,TRUE};
    HANDLE read=nullptr,write=nullptr;
    if(!CreatePipe(&read,&write,&inherit,0))return -1;
    SetHandleInformation(read,HANDLE_FLAG_INHERIT,0);
    HANDLE null=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&inherit,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdInput=null;startup.hStdOutput=startup.hStdError=write;
    auto command=quote_windows_arg(wit_.wstring())+L" "+arguments;
    auto environment=environment_with_locale();
    PROCESS_INFORMATION process{};
    const bool started=CreateProcessW(wit_.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,environment.data(),wit_.parent_path().c_str(),&startup,&process);
    CloseHandle(write);if(null!=INVALID_HANDLE_VALUE)CloseHandle(null);
    if(!started) {CloseHandle(read);return -1;}
    CloseHandle(process.hThread);
    {
        std::lock_guard lock(process_lock_);
        process_=process.hProcess;
        if(cancelled_)TerminateProcess(process_,1);
    }
    char buffer[4096];DWORD count=0;std::string tail;
    while(ReadFile(read,buffer,sizeof(buffer),&count,nullptr) && count) {
        output.append(buffer,count);
        if(track_progress) {
            tail.append(buffer,count);if(tail.size()>256)tail.erase(0,tail.size()-256);
            if(auto percent=parse_percent(tail))percent_=std::min(*percent,99);   // 100 is reported once main.dol is checked
        }
    }
    CloseHandle(read);
    WaitForSingleObject(process.hProcess,INFINITE);
    DWORD code=0;GetExitCodeProcess(process.hProcess,&code);
    std::lock_guard lock(process_lock_);
    process_=nullptr;CloseHandle(process.hProcess);
    return int(code);
}
void ImageUnpack::run() {
    const auto finish=[&](UnpackStatus status,UnpackError error=UnpackError::none) {error_=error;status_=status;};
    // Wit writes straight into game: renaming a staging folder fails while the antivirus or the indexer still scans the fresh files (sharing violation).
    const auto game=root_/L"game";
    std::error_code error;
    bool replaced=false;   // the old game folder is touched only after the image was identified
    try {
        if(!fs::is_regular_file(wit_,error))return finish(UnpackStatus::failed,UnpackError::wit_missing);
        if(inside(image_,game))return finish(UnpackStatus::failed,UnpackError::image_in_game_folder);
        std::string id;
        const int identified=wit(L"ID6 "+quote_windows_arg(image_.wstring()),id,false);
        if(stopped())return finish(UnpackStatus::cancelled);
        while(!id.empty() && std::isspace(static_cast<unsigned char>(id.back())))id.pop_back();
        // Wit prints the error text and exits 0 for a file that is not a disc image, so the ID decides.
        if(identified!=0 || id!=expected_id)return finish(UnpackStatus::failed,UnpackError::wrong_image);
        const auto space=fs::space(root_,error);
        if(!error && space.available+folder_size(game)<required_bytes_)return finish(UnpackStatus::failed,UnpackError::no_space);
        if(stopped())return finish(UnpackStatus::cancelled);   // the walk above can take a while; a cancel must not cost the old game
        replaced=true;
        fs::remove_all(game,error);
        if(error)return finish(UnpackStatus::failed,UnpackError::extract_failed);
        std::string output;
        const int extracted=wit(L"extract "+quote_windows_arg(image_.wstring())+L" "+quote_windows_arg(game.wstring())+L" --progress",output,true);
        const bool cancelled=stopped();
        std::error_code check_error;
        const bool has_dol=fs::is_regular_file(game/L"DATA"/L"sys"/L"main.dol",check_error);
        if(cancelled || extracted!=0 || !has_dol) {
            if(!cancelled)std::fprintf(stderr,"[unpack] failed: wit exit=%d main.dol=%d\n%s\n",extracted,int(has_dol),output.substr(output.size()>1500 ? output.size()-1500 : 0).c_str());
            fs::remove_all(game,error);
            return cancelled ? finish(UnpackStatus::cancelled) : finish(UnpackStatus::failed,UnpackError::extract_failed);
        }
        percent_=100;
        finish(UnpackStatus::done);
    } catch(...) {
        if(replaced)fs::remove_all(game,error);
        finish(UnpackStatus::failed,UnpackError::extract_failed);
    }
}
}
