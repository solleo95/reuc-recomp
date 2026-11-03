// hidden_run <program> [arguments...]: runs the program on a desktop of its own and returns its exit code. CMake puts it in front of every test (CMAKE_TEST_LAUNCHER), so the
// windows the tests open (the updater, the launcher, the menus) never appear on the user's desktop and never take the focus, whoever starts ctest and however.
// UC_TESTS_VISIBLE=1 runs the program on the real desktop instead. If the desktop cannot be made, the program runs where hidden_run runs.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <string>

int wmain() {
    // The command line after our own name, as it was typed: no re-quoting.
    const wchar_t* line = GetCommandLineW();
    const wchar_t* at = line;
    if (*at == L'"') { ++at; while (*at && *at != L'"') ++at; if (*at) ++at; }
    else while (*at && *at != L' ' && *at != L'\t') ++at;
    while (*at == L' ' || *at == L'\t') ++at;
    if (!*at) { std::fputs("usage: hidden_run <program> [arguments...]\n", stderr); return 2; }
    std::wstring command = at;

    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    HDESK desktop = nullptr;
    wchar_t* visible = nullptr;
    size_t visible_size = 0;
    if (_wdupenv_s(&visible, &visible_size, L"UC_TESTS_VISIBLE") == 0 && visible) { free(visible); }
    else {
        const std::wstring name = L"uc-tests-" + std::to_wstring(GetCurrentProcessId());
        desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!desktop) std::fprintf(stderr, "hidden_run: CreateDesktop failed (%lu), running on the visible desktop\n", GetLastError());
    }
    std::wstring desktop_name = L"winsta0\\uc-tests-" + std::to_wstring(GetCurrentProcessId());   // outlives CreateProcessW below
    if (desktop) startup.lpDesktop = desktop_name.data();
    // The program writes where we write (ctest reads our pipes): the standard handles go to it as they are.
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE); startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE); startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    for (HANDLE handle : {startup.hStdInput, startup.hStdOutput, startup.hStdError})
        if (handle && handle != INVALID_HANDLE_VALUE) SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process)) {
        std::fprintf(stderr, "hidden_run: cannot start the program (%lu)\n", GetLastError());
        if (desktop) CloseDesktop(desktop);
        return 3;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (desktop) CloseDesktop(desktop);
    return int(code);
}
