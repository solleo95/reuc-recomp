#include "test_pipe.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <chrono>
#include <stdexcept>
namespace uc {
namespace {
constexpr size_t kMaxLine = 4096, kMaxOut = 4096;
HANDLE make_pipe(const std::string& name, const char* suffix, DWORD access) {
    const std::wstring path = L"\\\\.\\pipe\\uc-test-" + std::wstring(name.begin(), name.end()) + std::wstring(suffix, suffix + std::char_traits<char>::length(suffix));
    HANDLE h = CreateNamedPipeW(path.c_str(), access | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                1, 65536, 65536, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("test pipe already in use: " + name);
    return h;
}
bool connect(HANDLE h) { return ConnectNamedPipe(h, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED; }
}  // namespace

TestPipes::TestPipes(const std::string& name) {
    cmd_ = make_pipe(name, "-cmd", PIPE_ACCESS_INBOUND);
    try { evt_ = make_pipe(name, "-evt", PIPE_ACCESS_OUTBOUND); } catch (...) { CloseHandle(cmd_); throw; }
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
    // CancelSynchronousIo misses a thread that has not entered its blocking call yet, so repeat until both threads left.
    while (!reader_done_ || !writer_done_) {
        if (!reader_done_) CancelSynchronousIo(reader_.native_handle());
        if (!writer_done_) CancelSynchronousIo(writer_.native_handle());
        out_cv_.notify_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    reader_.join(); writer_.join();
    CloseHandle(cmd_); CloseHandle(evt_);
}
void TestPipes::read_loop() {
    std::string partial; bool discarding = false;
    if (connect(cmd_)) {
        cmd_connected_ = true;
        char buffer[4096]; DWORD n = 0;
        while (!stop_ && ReadFile(cmd_, buffer, sizeof buffer, &n, nullptr) && n) {
            for (DWORD i = 0; i < n; ++i) {
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
    if (connect(evt_)) {
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
            DWORD written = 0;
            if (!WriteFile(evt_, line.data(), DWORD(line.size()), &written, nullptr) || written != line.size()) { if (!stop_) disconnected_ = true; break; }
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
