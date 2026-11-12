#include "test_pipe.h"
#include <cerrno>
#include <cstddef>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// Linux and macOS transport of the test server: two Unix-domain sockets, <runtime dir>/uc-test-<name>-cmd.sock (client -> game) and -evt.sock (game -> client),
// one client each. Windows uses named pipes (test_pipe_win.cpp). A remote harness reaches them with `ssh -L local.sock:remote.sock`. Android: abstract
// sockets "@uc-test-<name>-cmd" and "-evt" (adb's shell cannot reach the app's private folders): `adb forward tcp:<port> localabstract:uc-test-<name>-cmd`.
namespace uc {
namespace {
constexpr size_t kMaxLine = 4096, kMaxOut = 4096;
void* from_fd(int fd) { return reinterpret_cast<void*>(intptr_t(fd) + 1); }
int to_fd(void* value) { return int(reinterpret_cast<intptr_t>(value) - 1); }
bool abstract(const std::string& path) { return !path.empty() && path[0] == '@'; }   // Linux's abstract namespace: no file, gone with the process
sockaddr_un address_of(const std::string& path, socklen_t& length) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof address.sun_path) throw std::runtime_error("test socket path too long: " + path);
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    length = socklen_t(sizeof address);
    if (abstract(path)) {   // a leading NUL, and the name's exact length: adb's localabstract: connects to exactly that name
        address.sun_path[0] = '\0';
        length = socklen_t(offsetof(sockaddr_un, sun_path) + path.size());
    }
    return address;
}
// A close-on-exec stream socket and a close-on-exec accepted client. macOS has neither SOCK_CLOEXEC nor accept4: fcntl right after.
int stream_socket() {
#if defined(__APPLE__)
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
#else
    return ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
#endif
}
int accept_client(int listener) {
#if defined(__APPLE__)
    const int client = ::accept(listener, nullptr, nullptr);
    if (client >= 0) ::fcntl(client, F_SETFD, FD_CLOEXEC);
    return client;
#else
    return ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
#endif
}
std::string socket_path(const std::string& name, const char* suffix) {
#if defined(__ANDROID__)
    return "@uc-test-" + name + suffix;
#else
    const char* directory = std::getenv("XDG_RUNTIME_DIR");
    return std::string(directory && *directory ? directory : "/tmp") + "/uc-test-" + name + suffix + ".sock";
#endif
}
// Listens on `path`. `lock` receives a descriptor holding an flock on <path>.lock for as long as it stays open: a live game keeps it, so a second game
// of the same name fails here, while the socket file a dead game left behind is replaced. (A probe connection would land in the live game's accept queue.)
// An abstract name needs neither: a live game's name makes bind fail, a dead game's is gone.
int listen_on(const std::string& path, const std::string& name, int& lock) {
    socklen_t length = 0;
    const sockaddr_un address = address_of(path, length);
    lock = -1;
    if (!abstract(path)) {
        lock = ::open((path + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (lock < 0) throw std::runtime_error("cannot create the lock file of the test socket " + path);
        if (::flock(lock, LOCK_EX | LOCK_NB)) { ::close(lock); lock = -1; throw std::runtime_error("test pipe already in use: " + name); }
        ::unlink(path.c_str());
    }
    const int fd = stream_socket();
    if (fd < 0 || ::bind(fd, reinterpret_cast<const sockaddr*>(&address), length) || ::listen(fd, 1)) {
        if (fd >= 0) ::close(fd);
        if (lock >= 0) ::close(lock);
        lock = -1;
        throw std::runtime_error("cannot listen on the test socket " + path);
    }
    return fd;
}
}  // namespace

TestPipes::TestPipes(const std::string& name) {
    cmd_path_ = socket_path(name, "-cmd");
    evt_path_ = socket_path(name, "-evt");
    cmd_ = from_fd(listen_on(cmd_path_, name, cmd_lock_));
    try { evt_ = from_fd(listen_on(evt_path_, name, evt_lock_)); }
    catch (...) {
        ::close(to_fd(cmd_));
        if (!abstract(cmd_path_)) { ::unlink(cmd_path_.c_str()); ::unlink((cmd_path_ + ".lock").c_str()); ::close(cmd_lock_); }
        throw;
    }
    reader_ = std::thread([this] { read_loop(); });
    writer_ = std::thread([this] { write_loop(); });
}
TestPipes::~TestPipes() {
    {   // let queued replies (a final quit reply) reach a connected client
        const auto limit = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (evt_connected_ && !disconnected_ && std::chrono::steady_clock::now() < limit) {
            { std::lock_guard g(out_mutex_); if (out_.empty()) break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    stop_ = true; out_cv_.notify_all();
    // shutdown wakes a thread blocked in accept or read on the descriptor; repeat until both threads left (one may not have blocked yet)
    while (!reader_done_ || !writer_done_) {
        ::shutdown(to_fd(cmd_), SHUT_RDWR); ::shutdown(to_fd(evt_), SHUT_RDWR);
        if (const int c = cmd_client_.load(); c >= 0) ::shutdown(c, SHUT_RDWR);
        if (const int c = evt_client_.load(); c >= 0) ::shutdown(c, SHUT_RDWR);
        out_cv_.notify_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    reader_.join(); writer_.join();
    if (const int c = cmd_client_.exchange(-1); c >= 0) ::close(c);
    if (const int c = evt_client_.exchange(-1); c >= 0) ::close(c);
    ::close(to_fd(cmd_)); ::close(to_fd(evt_));
    if (abstract(cmd_path_)) return;   // nothing on disk
    ::unlink(cmd_path_.c_str()); ::unlink(evt_path_.c_str());
    ::unlink((cmd_path_ + ".lock").c_str()); ::unlink((evt_path_ + ".lock").c_str());   // removed while still locked, then released
    ::close(cmd_lock_); ::close(evt_lock_);
}
void TestPipes::read_loop() {
    std::string partial; bool discarding = false;
    const int client = accept_client(to_fd(cmd_));
    if (client >= 0) {
        cmd_client_ = client;
        cmd_connected_ = true;
        char buffer[4096];
        for (;;) {
            const ssize_t n = ::read(client, buffer, sizeof buffer);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0 || stop_) break;
            for (ssize_t i = 0; i < n; ++i) {
                const char c = buffer[i];
                if (c == '\n') {
                    if (!partial.empty() && partial.back() == '\r') partial.pop_back();
                    std::lock_guard g(in_mutex_);
                    in_lines_.push_back(discarding ? "\x01too-long" : std::move(partial));
                    partial.clear(); discarding = false;
                } else if (!discarding) {
                    partial += c;
                    if (partial.size() > kMaxLine) { discarding = true; partial.clear(); }
                }
            }
        }
        if (!stop_) disconnected_ = true;
    }
    reader_done_ = true;
}
void TestPipes::write_loop() {
    const int client = accept_client(to_fd(evt_));
    if (client >= 0) {
        evt_client_ = client;
        evt_connected_ = true;
        while (!stop_) {
            std::string line;
            {
                std::unique_lock lk(out_mutex_);
                out_cv_.wait(lk, [this] { return stop_ || !out_.empty(); });
                if (stop_) break;
                line = std::move(out_.front().first); out_.pop_front();
            }
            line += '\n';
            size_t done = 0;
            while (done < line.size()) {
                const ssize_t n = ::send(client, line.data() + done, line.size() - done, MSG_NOSIGNAL);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) { if (!stop_) disconnected_ = true; break; }
                done += size_t(n);
            }
            if (done < line.size()) break;
        }
    }
    writer_done_ = true;
}
std::vector<std::string> TestPipes::take_lines() { std::lock_guard g(in_mutex_); return std::exchange(in_lines_, {}); }
void TestPipes::send(std::string line, bool droppable) {
    {
        std::lock_guard g(out_mutex_);
        if (out_.size() >= kMaxOut) {
            for (auto it = out_.begin(); it != out_.end(); ++it) if (it->second) { out_.erase(it); ++dropped_; break; }
        }
        out_.emplace_back(std::move(line), droppable);   // replies are kept even when no frame line could be dropped
    }
    out_cv_.notify_one();
}
bool TestPipes::connected() const { return cmd_connected_ && evt_connected_; }
bool TestPipes::disconnected() const { return disconnected_; }
uint64_t TestPipes::dropped() const { return dropped_; }
}  // namespace uc
