#include "windows_path.h"
#include <windows.h>

namespace uc {
namespace {
std::wstring final_path(const std::wstring& existing) {
    HANDLE h=CreateFileW(existing.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,
        OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(h==INVALID_HANDLE_VALUE) return {};
    std::wstring result;
    for(DWORD volume:{VOLUME_NAME_DOS,VOLUME_NAME_GUID}) {   // a drive mounted only in a folder has no drive letter: the GUID name
        std::wstring text(MAX_PATH,L'\0');
        for(;;) {
            auto length=GetFinalPathNameByHandleW(h,text.data(),static_cast<DWORD>(text.size()),FILE_NAME_NORMALIZED|volume);
            if(!length) break;
            if(length<text.size()) {text.resize(length);result=std::move(text);break;}
            text.resize(length);
        }
        if(!result.empty()) break;
    }
    CloseHandle(h);
    if(result.starts_with(L"\\\\?\\UNC\\")) result.replace(0,8,L"\\\\");
    else if(result.size()>=6 && result.starts_with(L"\\\\?\\") && result[5]==L':') result.erase(0,4);
    return result;
}
}
std::wstring resolve_folder(const std::filesystem::path& folder) {
    const auto absolute=std::filesystem::absolute(folder).lexically_normal();
    auto existing=absolute;
    std::filesystem::path missing;
    for(;;) {
        if(auto real=final_path(existing.wstring()); !real.empty()) {
            std::filesystem::path result(real);
            if(!missing.empty()) result/=missing;
            return result.wstring();
        }
        const auto parent=existing.parent_path();
        if(parent==existing || parent.empty()) return absolute.wstring();
        missing=missing.empty() ? existing.filename() : existing.filename()/missing;
        existing=parent;
    }
}
size_t volume_root_length(const std::wstring& path) {
    size_t position=0,names=1;   // names: the drive or volume, or server and share
    if(path.starts_with(L"\\\\?\\")) {
        position=4;
        if(path.compare(position,4,L"UNC\\")==0) {position+=4;names=2;}
    } else if(path.starts_with(L"\\\\")) {position=2;names=2;}
    for(;names;--names) {
        position=path.find(L'\\',position);
        if(position==std::wstring::npos) return path.size();
        ++position;
    }
    return position;
}
}
