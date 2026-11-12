// The Linux test-server transport: lines in on one socket, lines out on the other, a second game with the same name refused, clean shutdown.
#ifndef __linux__
int main() { return 0; }
#else
#include "check.h"
#include "test_pipe.h"
#include "test_pid.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

static int connect_to(const std::string& path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof address.sun_path - 1);
    for (int attempt = 0; attempt < 200; ++attempt) {   // the game's threads start listening a moment after the constructor returns
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0) return fd;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return -1;
}

int main() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
#if defined(__ANDROID__)   // the game's rule (game/test_pipe_posix.cpp): no /tmp there, the temporary folder instead
    if (!runtime || !*runtime) runtime = std::getenv("TMPDIR");
#endif
    const std::string dir = runtime && *runtime ? runtime : "/tmp";
    const std::string name = "pipetest" + std::to_string(test_process_id());
    int command = -1, events = -1;
    {
        uc::TestPipes pipes(name);
        bool refused = false;
        try { uc::TestPipes second(name); } catch (const std::runtime_error&) { refused = true; }
        CHECK(refused);

        command = connect_to(dir + "/uc-test-" + name + "-cmd.sock");
        events = connect_to(dir + "/uc-test-" + name + "-evt.sock");
        CHECK(command >= 0 && events >= 0);
        CHECK(::write(command, "ping\r\nsecond\n", 13) == 13);
        std::vector<std::string> lines;
        for (int i = 0; i < 400 && lines.size() < 2; ++i) {
            for (auto& line : pipes.take_lines()) lines.push_back(line);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(lines.size() == 2 && lines[0] == "ping" && lines[1] == "second");   // the carriage return is dropped
        CHECK(pipes.connected() && !pipes.disconnected());

        pipes.send("reply one", false);
        pipes.send("frame", true);
        char text[64]{};
        size_t got = 0;
        while (got < 16) {
            const ssize_t n = ::read(events, text + got, sizeof text - 1 - got);
            if (n <= 0) break;
            got += size_t(n);
        }
        CHECK(std::string(text) == "reply one\nframe\n");

        ::close(command);   // the client goes away
        command = -1;
        for (int i = 0; i < 400 && !pipes.disconnected(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(pipes.disconnected());
    }   // the destructor must return even though the event client is still connected
    ::close(events);
    CHECK(::access((dir + "/uc-test-" + name + "-cmd.sock").c_str(), F_OK) != 0);   // the sockets are removed
    return 0;
}
#endif
