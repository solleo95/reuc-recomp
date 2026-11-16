#include "launch.h"
#include "options_config.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <vector>
#include <fcntl.h>
#include <fstream>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

// Linux: the GPU feature probe and the game launch of launch.cpp. The game asks its own executable for the report, as the Windows launcher does, because a
// driver or runtime that hangs in the probe must not take the game down with it.
namespace uc {
// The handle of a game process: its pid, and whether it is already reaped (a reaped pid may be given to another child later; it is never asked about again).
// Nothing is killed on close (the game outlives the launcher, as on Windows), only a finished child is reaped.
namespace {
struct PosixProcess { pid_t pid; std::atomic<bool> finished{false}; };
}
void ProcessCloser::operator()(void* handle) const {
    auto* process = static_cast<PosixProcess*>(handle);
    if (!process) return;
    if (!process->finished) { int status = 0; waitpid(process->pid, &status, WNOHANG); }
    delete process;
}
bool game_running(const GameProcess& handle) {
    auto* process = static_cast<PosixProcess*>(handle.get());
    if (!process || process->finished) return false;
    int status = 0;
    if (waitpid(process->pid, &status, WNOHANG) == 0) return true;   // still running
    process->finished = true;                                         // reaped now, or already gone (ECHILD)
    return false;
}
GameProcess launch_game(const std::filesystem::path& executable, const std::filesystem::path& config, const Options* diagnostic, int guest_cpu, std::optional<net::Endpoint> network, std::string_view ui) {
    if (guest_cpu < -1 || guest_cpu >= 63) throw std::invalid_argument("guest CPU must be -1 or 0..62");
    const auto exe = std::filesystem::absolute(executable), settings = std::filesystem::absolute(config);
    if (!std::filesystem::is_regular_file(exe)) throw std::runtime_error("uc is missing");
    if (!std::filesystem::is_regular_file(settings)) throw std::runtime_error("config.json is missing");
    const auto options = options_from_config(load_config(settings));
    if (!std::filesystem::is_regular_file(options.game / "DATA" / "sys" / "main.dol")) throw std::runtime_error("game folder does not contain DATA/sys/main.dol");
    std::vector<std::string> arguments{exe.string(), "--config", settings.string()};
    if (network) {
        if (!network->port || (!network->host && !net::valid_address(network->address))) throw std::invalid_argument("invalid network endpoint");
        if (network->host) arguments.emplace_back("--net-host");
        else { arguments.emplace_back("--net-join"); arguments.push_back(network->address); }
        arguments.emplace_back("--net-port"); arguments.push_back(std::to_string(network->port));
        if (network->loopback) arguments.emplace_back("--net-loopback");
    }
    if (guest_cpu >= 0) { arguments.emplace_back("--guest-cpu"); arguments.push_back(std::to_string(guest_cpu)); }
    if (!ui.empty()) { arguments.emplace_back("--ui"); arguments.emplace_back(ui); }
    if (diagnostic) {
        if (diagnostic->test_control.empty() || diagnostic->save_root.empty()) throw std::invalid_argument("diagnostic launch requires isolated input/save paths");
        arguments.emplace_back("--test-control"); arguments.push_back(diagnostic->test_control.string());
        arguments.emplace_back("--save-root"); arguments.push_back(diagnostic->save_root.string());
        for (const char* item : {"--seconds", "20", "--skip-intro", "--window-mode", "windowed", "--width", "1280", "--height", "720", "--render-scale", "1", "--fps-limit", "0", "--mute"}) arguments.emplace_back(item);
    }
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    std::vector<std::string> environment;   // "Enable logs": a release client writes no logs unless UC_LOGS is set (common/public_release.h)
    std::vector<char*> envp;
    for (char** item = environ; *item; ++item) if (!options.logs || std::strncmp(*item, "UC_LOGS=", 8) != 0) envp.push_back(*item);
    if (options.logs) { environment.emplace_back("UC_LOGS=1"); envp.push_back(environment.back().data()); }
    envp.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    const auto folder = exe.parent_path().string();
#if !defined(__ANDROID__) || __ANDROID_API__ >= 34
    posix_spawn_file_actions_addchdir_np(&actions, folder.c_str());
#endif
    pid_t child = 0;
    const int error = posix_spawn(&child, exe.c_str(), &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    if (error != 0) throw std::runtime_error(std::string("posix_spawn failed: ") + std::strerror(error));
    std::fprintf(stderr, "[launch] game child pid=%d network=%s port=%u\n", int(child), network ? (network->host ? "host" : "client") : "off", network ? network->port : 0);
    auto* process = new PosixProcess;
    process->pid = child;
    return GameProcess(process);
}
std::string last_json_line(std::string_view output) {   // declared in launch.h; launch.cpp defines it for Windows
    std::string result;
    for (size_t begin = 0; begin < output.size();) {
        auto end = output.find('\n', begin);
        if (end == std::string_view::npos) end = output.size();
        auto line = output.substr(begin, end - begin);
        begin = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.size() > 1 && line.front() == '{' && line.back() == '}') result = std::string(line);
    }
    return result;
}

std::string query_features(const std::filesystem::path& executable, const std::filesystem::path& config, unsigned timeout_ms, std::optional<RendererBackend> backend) {
    if (backend && renderer_backend_name(*backend).empty()) throw std::invalid_argument("invalid feature-query renderer");
    const auto exe = std::filesystem::absolute(executable);
    if (!std::filesystem::is_regular_file(exe)) return {};
    const auto output = std::filesystem::temp_directory_path() / ("uc-features-" + std::to_string(getpid()) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
    std::vector<std::string> arguments{exe.string(), "--features", "--config", std::filesystem::absolute(config).string()};
    if (backend) { arguments.push_back("--renderer"); arguments.emplace_back(renderer_backend_name(*backend)); }
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 1, output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    pid_t child = 0;
    std::string text;
    if (posix_spawn(&child, exe.c_str(), &actions, nullptr, argv.data(), environ) == 0) {
        std::fprintf(stderr, "[launch] GPU probe child pid=%d timeout_ms=%u\n", int(child), timeout_ms);
        // The report is read as soon as it is written: the child's exit (driver teardown) is not waited for.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        bool reported = false, exited = false;
        int status = 0;
        while (!exited && !reported && std::chrono::steady_clock::now() < deadline) {
            exited = waitpid(child, &status, WNOHANG) == child;
            std::ifstream in(output, std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(in), {});
            reported = !last_json_line(text).empty();
            if (!exited && !reported) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!exited) {
            if (!reported) std::fprintf(stderr, "[launch] GPU probe timed out\n");
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
        }
        std::fprintf(stderr, "[launch] GPU probe exit=%d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
    posix_spawn_file_actions_destroy(&actions);
    { std::ifstream in(output, std::ios::binary); text.assign(std::istreambuf_iterator<char>(in), {}); }
    std::error_code ignored;
    std::filesystem::remove(output, ignored);
    return last_json_line(text);
}
}  // namespace uc
