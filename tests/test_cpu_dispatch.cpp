// uc.exe of the release (game/cpu_dispatch.cpp) starts uc_avx2.exe or uc_compat.exe and passes the arguments and the exit code on.
// This program is the parent (argument: the dispatcher) and, copied under the names of the two game programs, the child.
#include "check.h"
#include <cstdlib>
#include <filesystem>
#include <string>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
namespace fs = std::filesystem;

static DWORD run(const fs::path& program, const wchar_t* tail) {
    std::wstring command = L"\"" + program.wstring() + L"\" " + tail;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    CHECK(CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process));
    WaitForSingleObject(process.hProcess, 30000);
    DWORD code = 999;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return code;
}

int wmain(int argc, wchar_t** argv) {
    const std::wstring self = fs::path(argv[0]).filename().wstring();
    if (self == L"uc_avx2.exe" || self == L"uc_compat.exe") {   // the child: exit code 41/42 says who it is, 50 a wrong argument list
        if (argc != 3 || std::wstring(argv[1]) != L"a b" || std::wstring(argv[2]) != L"--config=x y") return 50;
        return self == L"uc_avx2.exe" ? 41 : 42;
    }
    CHECK(argc == 2);
    const fs::path folder = fs::current_path() / "gm_cpu_dispatch";
    fs::remove_all(folder);
    fs::create_directories(folder);
    fs::copy_file(argv[1], folder / "uc.exe");
    fs::copy_file(argv[0], folder / "uc_avx2.exe");
    fs::copy_file(argv[0], folder / "uc_compat.exe");
    const wchar_t* tail = L"\"a b\" --config=\"x y\"";
    _wputenv_s(L"UC_COMPAT_CPU", L"1");
    CHECK(run(folder / "uc.exe", tail) == 42);   // the compatible program on any CPU, arguments untouched, exit code passed on
    _wputenv_s(L"UC_COMPAT_CPU", L"0");
    const DWORD chosen = run(folder / "uc.exe", tail);
    CHECK(chosen == 41 || chosen == 42);   // by this CPU
    fs::remove_all(folder);
    return 0;
}
