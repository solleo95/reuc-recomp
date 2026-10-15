#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
namespace uc {
// Win32 transport of the test server: two single-client pipes carrying lines.
//   \\.\pipe\uc-test-<name>-cmd  client -> game      \\.\pipe\uc-test-<name>-evt  game -> client
class TestPipes {
public:
    explicit TestPipes(const std::string& name);     // creates both pipes, starts reader/writer threads
    ~TestPipes();                                    // drains replies briefly, cancels blocking I/O, joins
    TestPipes(const TestPipes&) = delete; TestPipes& operator=(const TestPipes&) = delete;
    std::vector<std::string> take_lines();           // complete command lines received since last call
    void send(std::string line, bool droppable);     // droppable = frame event; queue <= 4096, drops oldest droppable
    bool connected() const;                          // both ends connected
    bool disconnected() const;                       // a connected end failed
    uint64_t dropped() const;
private:
    void read_loop(); void write_loop();
    void* cmd_ = nullptr; void* evt_ = nullptr;
    std::atomic<bool> stop_{false}, cmd_connected_{false}, evt_connected_{false}, disconnected_{false};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<bool> reader_done_{false}, writer_done_{false};
    std::mutex in_mutex_, out_mutex_;
    std::condition_variable out_cv_;
    std::vector<std::string> in_lines_;
    std::deque<std::pair<std::string, bool>> out_;
    std::thread reader_, writer_;
};
}
