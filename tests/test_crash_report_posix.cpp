// Linux crash handling: a fault or fatal() writes the crash report and ends the process with the documented status, in any thread, without looping.
#ifndef __linux__
int main() { return 0; }   // the Windows handlers are covered by test_crash_report
#else
#include "check.h"
#include "clock.h"
#include "crash_report.h"
#include "fatal.h"
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

enum class Mode { null_in_guest_thread, null_in_other_thread, fatal_call, abort_call };

// Runs `mode` in a child with the handlers installed; returns the child's wait status. The report directory is `dir`.
static int run_child(const fs::path& dir, Mode mode) {
    const pid_t child = fork();
    if (child == 0) {
        uc::crash_report_init(dir, [] { return std::string("test state"); });
        uc::install_crash_handlers();
        const auto fault = [] { volatile int* p = nullptr; *p = 1; };
        switch (mode) {
        case Mode::null_in_guest_thread: uc::mark_guest_thread(); fault(); break;
        case Mode::null_in_other_thread: { std::thread t([&] { uc::crash_guard_thread(); fault(); }); t.join(); break; }
        case Mode::fatal_call: uc::fatal("test fatal %d", 7); break;
        case Mode::abort_call: std::abort(); break;
        }
        _exit(0);   // reaching this is a failure: the handler did not end the process
    }
    int status = 0;
    waitpid(child, &status, 0);
    return status;
}

static std::string newest_report(const fs::path& dir) {
    fs::path newest;
    for (const auto& entry : fs::directory_iterator(dir))
        if (entry.path().filename().string().rfind("crash-", 0) == 0 && (newest.empty() || entry.path() > newest)) newest = entry.path();
    CHECK(!newest.empty());
    std::ifstream in(newest);
    std::stringstream text; text << in.rdbuf();
    return text.str();
}

int main() {
    const fs::path root = fs::temp_directory_path() / ("uc-crash-test-" + std::to_string(getpid()));
    for (const Mode mode : {Mode::null_in_guest_thread, Mode::null_in_other_thread, Mode::fatal_call, Mode::abort_call}) {
        const fs::path dir = root / std::to_string(int(mode));
        fs::create_directories(dir);
        const auto started = std::chrono::steady_clock::now();
        const int status = run_child(dir, mode);
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));   // a looping handler would hang here
        CHECK(WIFEXITED(status));
        const std::string report = newest_report(dir);
        std::printf("mode %d: exit %d\n", int(mode), WEXITSTATUS(status));
        if (mode == Mode::fatal_call) { CHECK(WEXITSTATUS(status) == 3); CHECK(report.find("fatal: test fatal 7") != std::string::npos); }
        else { CHECK(WEXITSTATUS(status) == 139); CHECK(report.find("SIGSEGV") != std::string::npos || report.find("SIGABRT") != std::string::npos); }
        CHECK(report.find("test state") != std::string::npos);
    }
    fs::remove_all(root);
    return 0;
}
#endif
