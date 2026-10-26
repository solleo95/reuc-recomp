#pragma once
#include <filesystem>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace uc {
// True for an installed release client: release.json (the update manifest) sits next to the executable. Such a client writes no logs, crash reports or
// debug files. A development build (build\, dist\, the install folder of build.bat) has no release.json, so it keeps all of them.
// The environment variable UC_LOGS (any value) turns the logs back on in a release client, for developers and for tests of the update flow.
inline bool public_release() {
    static const bool value=[] {
        if(GetEnvironmentVariableW(L"UC_LOGS",nullptr,0))return false;
        wchar_t path[32768];
        const DWORD size=GetModuleFileNameW(nullptr,path,32768);
        if(!size || size>=32768)return false;
        std::error_code ignored;
        return std::filesystem::is_regular_file(std::filesystem::path(path).parent_path()/L"release.json",ignored);
    }();
    return value;
}
}
