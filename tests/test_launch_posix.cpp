// Linux/Deck only (tests/linux/CMakeLists.txt): launch_game starts the `uc` next to the config, in its folder, with the arguments the Windows launcher passes
// (--config, --guest-cpu, network, --ui), and refuses a missing program, a missing config or a game folder without DATA/sys/main.dol.
#include "check.h"
#include "launch.h"
#include "options_config.h"
#include "platform.h"
#include <chrono>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
namespace fs = std::filesystem;

static void write(const fs::path& path, std::string_view text, mode_t mode) {
    fs::create_directories(path.parent_path()); { std::ofstream(path, std::ios::binary) << text; } ::chmod(path.c_str(), mode);
}
static std::string read(const fs::path& path) { std::ifstream in(path, std::ios::binary); std::ostringstream text; text << in.rdbuf(); return text.str(); }
static bool wait_exit(const uc::GameProcess& process) {
    for (int i = 0; i < 100; ++i) { if (!uc::game_running(process)) return true; std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    return false;
}
template<class F> static bool throws(F&& f) { try { f(); } catch (const std::exception&) { return true; } return false; }

int main(int, char** argv) {
    CHECK(fs::is_regular_file(uc::platform::executable_dir() / fs::path(argv[0]).filename()));   // the folder of this very program
    CHECK(std::string_view(uc::platform::game_program()) == "uc");
    ::unsetenv("UC_LOGS");   // the "logs off" cases below pass this environment through unchanged
    CHECK(uc::platform::path_from_wide(L"/tmp/é日\U0001F600").string() == "/tmp/\xC3\xA9\xE6\x97\xA5\xF0\x9F\x98\x80");   // wide arguments reach the filesystem as UTF-8
    CHECK(uc::platform::utf8_to_wide("A\xC3\xA9\xE6\x97\xA5\xF0\x9F\x98\x80", true) == std::optional<std::wstring>(std::wstring{L'A', 0xE9, 0x65E5, 0x1F600}));
    CHECK(uc::platform::utf8_to_wide("", true) == std::optional<std::wstring>(std::wstring{}));
    CHECK(!uc::platform::utf8_to_wide("\xC3", true) && !uc::platform::utf8_to_wide("\xC3(", true) && !uc::platform::utf8_to_wide("\xFF", true));
    CHECK(uc::platform::utf8_to_wide("a\xC3", false) == std::optional<std::wstring>(std::wstring{L'a', 0xFFFD}));
    const auto root = fs::temp_directory_path() / ("uc-launch-posix-" + std::to_string(::getpid()));
    fs::remove_all(root);
    write(root / "uc", "#!/bin/sh\npwd > cwd.txt\nfor a in \"$@\"; do echo \"$a\"; done > args.txt\nenv | grep '^UC_LOGS=' > env.txt\nexit 0\n", 0755);
    write(root / "game/DATA/sys/main.dol", "x", 0644);
    uc::Options options; options.game = root / "game";
    uc::ConfigDocument document; uc::store_options(document, options); uc::save_config(root / "config.json", document);

    auto process = uc::launch_game(root / "uc", root / "config.json", nullptr, 3, std::nullopt, "deck");
    CHECK(process);
    CHECK(wait_exit(process));
    const auto args = read(root / "args.txt");
    CHECK(args.find("--config\n" + fs::absolute(root / "config.json").string() + "\n") != std::string::npos);
    CHECK(args.find("--guest-cpu\n3\n") != std::string::npos);
    CHECK(args.find("--ui\ndeck\n") != std::string::npos);
    CHECK(fs::equivalent(fs::path(read(root / "cwd.txt").substr(0, read(root / "cwd.txt").find('\n'))), root));   // started in the program's folder
    CHECK(read(root / "env.txt").empty());                                                                         // logs are off: no UC_LOGS

    uc::net::Endpoint endpoint; endpoint.host = true; endpoint.port = 4000;
    auto hosting = uc::launch_game(root / "uc", root / "config.json", nullptr, -1, endpoint, {});
    CHECK(wait_exit(hosting));
    const auto host_args = read(root / "args.txt");
    CHECK(host_args.find("--net-host\n") != std::string::npos && host_args.find("--net-port\n4000\n") != std::string::npos);
    CHECK(host_args.find("--ui") == std::string::npos && host_args.find("--guest-cpu") == std::string::npos);

    options.logs = true; document = uc::load_config(root / "config.json"); uc::store_options(document, options); uc::save_config(root / "config.json", document);   // saving needs the document as loaded
    auto logging = uc::launch_game(root / "uc", root / "config.json", nullptr, -1, std::nullopt, {});
    CHECK(wait_exit(logging));
    CHECK(read(root / "env.txt").find("UC_LOGS=1") == 0);

    CHECK(throws([&] { uc::launch_game(root / "missing", root / "config.json", nullptr, -1, std::nullopt, {}); }));
    CHECK(throws([&] { uc::launch_game(root / "uc", root / "no-config.json", nullptr, -1, std::nullopt, {}); }));
    CHECK(throws([&] { uc::launch_game(root / "uc", root / "config.json", nullptr, 63, std::nullopt, {}); }));
    fs::remove(root / "game/DATA/sys/main.dol");
    CHECK(throws([&] { uc::launch_game(root / "uc", root / "config.json", nullptr, -1, std::nullopt, {}); }));
    CHECK(!uc::game_running(uc::GameProcess{}));
    CHECK(!uc::game_running(process) && !uc::game_running(process));   // a reaped game stays "not running" however often it is asked
    fs::remove_all(root);
    return 0;
}
