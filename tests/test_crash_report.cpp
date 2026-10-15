#include "check.h"
#include "crash_report.h"
#include "dispatch.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <atomic>
#include <chrono>
#include <thread>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// A throw while writing must not re-enter terminate from inside the crash path.
static_assert(noexcept(uc::crash_report_write(std::string_view{}, nullptr)));

static bool g_written = false;
__declspec(noinline) static void fn_a(ppc::Ctx&) {
    // A report written from inside a translated function names that function's guest address.
    g_written = uc::crash_report_write("synthetic crash for the test", nullptr);
}
__declspec(noinline) static void fn_b(ppc::Ctx&) {}

int main() {
    const uc::FnEntry table[] = {{0x80001000, fn_a}, {0x80002000, fn_b}};
    uc::dispatch_init(table, 2, 0x80001000, 0x80003000);
    CHECK(uc::guest_function_for_host(reinterpret_cast<const char*>(&fn_a) + 1) == 0x80001000);
    int local = 0;
    CHECK(uc::guest_function_for_host(&local) == 0); // a stack address is not translated code

    const auto dir = std::filesystem::temp_directory_path() / "uc-crash-report-test";
    std::filesystem::create_directories(dir);
    for (const auto& entry : std::filesystem::directory_iterator(dir)) if (entry.is_regular_file()) std::filesystem::remove(entry);
    uc::crash_report_init(dir, [] { return std::string("retraces 42"); });
    ppc::Ctx ctx{};
    uc::call_indirect(ctx, 0x80001000);
    CHECK(g_written);
    // Only the first crash is reported; later and concurrent attempts write nothing.
    bool second = true, third = true;
    std::thread t1([&] { second = uc::crash_report_write("second", nullptr); });
    std::thread t2([&] { third = uc::crash_report_write("third", nullptr); });
    t1.join(); t2.join();
    CHECK(!second && !third);
    size_t files = 0; std::string text;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        ++files;
        std::ifstream in(entry.path()); std::stringstream s; s << in.rdbuf(); text = s.str();
        CHECK(entry.path().filename().string().rfind("crash-", 0) == 0);
    }
    CHECK(files == 1);
    CHECK(text.find("synthetic crash for the test") != std::string::npos);
    CHECK(text.find("guest 80001000") != std::string::npos);
    CHECK(text.find("retraces 42") != std::string::npos);

    // A thread that loses the race waits for the winner's report instead of exiting the process under it.
    using clock = std::chrono::steady_clock;
    std::atomic<clock::time_point> stats_end{}, loser_returned{};
    std::thread loser;
    uc::crash_report_init(std::filesystem::temp_directory_path() / "uc-crash-report-race", [&] {
        loser = std::thread([&] { uc::crash_report_write("loser", nullptr); loser_returned = clock::now(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        stats_end = clock::now();
        return std::string("slow stats");
    });
    CHECK(uc::crash_report_write("winner", nullptr));
    loser.join();
    CHECK(loser_returned.load() >= stats_end.load());

    // The bundle hook runs after the report with <report stem>.dump next to it; the report names the folder; a throwing hook loses nothing.
    for (const bool throws : {false, true}) {
        const auto bundle_dir = std::filesystem::temp_directory_path() / (throws ? "uc-crash-bundle-throws" : "uc-crash-bundle");
        std::filesystem::remove_all(bundle_dir);
        uc::crash_report_init(bundle_dir, [] { return std::string("state"); });
        std::filesystem::path got;
        uc::crash_report_set_bundle([&](const std::filesystem::path& p) {
            got = p; std::filesystem::create_directories(p);
            if (throws) throw std::runtime_error("disk full");
        });
        CHECK(uc::crash_report_write("with bundle", nullptr));
        CHECK(got.extension() == ".dump" && got.parent_path() == bundle_dir && std::filesystem::is_directory(got));
        std::string report;
        for (const auto& entry : std::filesystem::directory_iterator(bundle_dir)) if (entry.is_regular_file()) { std::ifstream in(entry.path()); std::stringstream s; s << in.rdbuf(); report = s.str(); CHECK(got.stem() == entry.path().stem()); }
        CHECK(report.find("bundle: " + got.string()) != std::string::npos);
    }

    // Host threads reserve stack for the handler so a stack overflow can still be reported.
    ULONG guarantee = 0;
    std::thread([&] { uc::crash_guard_thread(); SetThreadStackGuarantee(&guarantee); }).join();
    CHECK(guarantee >= 64 * 1024);
    return 0;
}
