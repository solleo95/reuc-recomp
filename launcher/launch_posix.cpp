#include "launch.h"
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

// Linux: only the GPU feature probe of launch.cpp (the launcher program itself is Windows-only). The game asks its own executable for the report, as the
// Windows launcher does, because a driver or runtime that hangs in the probe must not take the game down with it.
namespace uc {
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
