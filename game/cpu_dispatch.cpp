// uc.exe of the Windows release: starts the game program this CPU can run. uc_avx2.exe is built for x86-64-v3 (AVX2, FMA, BMI), uc_compat.exe for x86-64-v2 (SSE4.2).
// Built for the plain x86-64 baseline and with nothing but the Windows API, so it starts on every CPU. The arguments, the working folder, the handles and the exit code pass through.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <string>

namespace {
// The x86-64-v3 feature set (what -march=x86-64-v3 may emit) plus the operating system saving the ymm registers.
bool cpu_runs_v3() {
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7) return false;
    __cpuid(r, 1);
    const unsigned c1 = unsigned(r[2]), need1 = (1u << 12) | (1u << 22) | (1u << 27) | (1u << 28) | (1u << 29);   // FMA, MOVBE, OSXSAVE, AVX, F16C
    if ((c1 & need1) != need1) return false;
    __cpuidex(r, 7, 0);
    const unsigned b7 = unsigned(r[1]), need7 = (1u << 3) | (1u << 5) | (1u << 8);   // BMI1, AVX2, BMI2
    if ((b7 & need7) != need7) return false;
    __cpuid(r, int(0x80000000));
    if (unsigned(r[0]) < 0x80000001u) return false;
    __cpuid(r, int(0x80000001));
    if (!(unsigned(r[2]) & (1u << 5))) return false;   // LZCNT
    return (GetEnabledXStateFeatures() & 4) != 0;   // XSTATE_MASK_AVX
}
void fail(const std::wstring& text) {
    MessageBoxW(nullptr, text.c_str(), L"Umbrella Chronicles", MB_OK | MB_ICONERROR);
}
}

int wmain() {
    wchar_t path[32768];
    const DWORD length = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
    if (length == 0 || length >= std::size(path)) return 1;
    std::wstring game(path, length);
    game.resize(game.find_last_of(L'\\') + 1);
    wchar_t force[2]{};
    const bool compat = GetEnvironmentVariableW(L"UC_COMPAT_CPU", force, 2) > 0 && force[0] != L'0';   // tests: take the compatible program on any CPU
    game += (compat || !cpu_runs_v3()) ? L"uc_compat.exe" : L"uc_avx2.exe";
    // The command line after our own name, as it was typed.
    const wchar_t* rest = GetCommandLineW();
    if (*rest == L'"') {
        for (++rest; *rest && *rest != L'"'; ++rest) {}
        if (*rest) ++rest;
    } else {
        while (*rest && *rest != L' ' && *rest != L'\t') ++rest;
    }
    std::wstring command = L"\"" + game + L"\"" + rest;
    STARTUPINFOW startup{};
    GetStartupInfoW(&startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(game.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &startup, &process)) {
        const DWORD error = GetLastError();
        fail(game.substr(game.find_last_of(L'\\') + 1) + L" cannot be started (Windows error " + std::to_wstring(error) + L"). Reinstall the game or run the launcher's update.");
        return 1;
    }
    // Ending this program ends the game with it. A failure (a job we already belong to cannot nest on old Windows) leaves the game running on its own.
    if (HANDLE job = CreateJobObjectW(nullptr, nullptr)) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) AssignProcessToJobObject(job, process.hProcess);
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    return int(code);
}
