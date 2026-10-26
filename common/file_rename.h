#pragma once
#include <algorithm>
#include <string_view>
#include <system_error>
#include <vector>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>

namespace uc {
inline std::error_code rename_sibling_file(HANDLE file,std::wstring_view filename,bool replace) {
    if(filename.empty() || filename.find_first_of(L"/\\:")!=filename.npos)
        return {ERROR_INVALID_NAME,std::system_category()};
    // A relative NT rename uses the source parent without reopening the pinned directory for write.
    struct Rename { BOOLEAN replace; HANDLE root; ULONG length; wchar_t name[1]; };
    const size_t size=offsetof(Rename,name)+filename.size()*sizeof(wchar_t);
    std::vector<unsigned char> storage(size,0);
    auto* info=reinterpret_cast<Rename*>(storage.data());
    info->replace=replace;info->length=static_cast<ULONG>(filename.size()*sizeof(wchar_t));
    std::copy(filename.begin(),filename.end(),info->name);
    using SetInformation=NTSTATUS(NTAPI*)(HANDLE,PIO_STATUS_BLOCK,PVOID,ULONG,ULONG);
    using StatusToError=ULONG(WINAPI*)(NTSTATUS);
    const auto library=GetModuleHandleW(L"ntdll.dll");
    auto set=reinterpret_cast<SetInformation>(GetProcAddress(library,"NtSetInformationFile"));
    auto translate=reinterpret_cast<StatusToError>(GetProcAddress(library,"RtlNtStatusToDosError"));
    if(!set || !translate)return {ERROR_PROC_NOT_FOUND,std::system_category()};
    IO_STATUS_BLOCK io{};
    constexpr ULONG file_rename_information=10; // Omitted from user-mode winternl.h's truncated enum.
    const auto status=set(file,&io,info,static_cast<ULONG>(size),file_rename_information);
    if(status<0)return {int(translate(status)),std::system_category()};
    if(!FlushFileBuffers(file))return {int(GetLastError()),std::system_category()};
    return {};
}
}
