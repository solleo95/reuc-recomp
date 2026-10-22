#pragma once
#include "native_input.h"
#include "test_pipe.h"
#include "test_protocol.h"
#include <cstdint>
#include <deque>
#include <set>
#include <string>
#include <vector>
namespace uc {
struct TestInputSink {                     // implemented over NativeInput in uc_main, a fake in tests
    virtual ~TestInputSink() = default;
    virtual bool kpad(uint64_t sequence, float x, float y, uint32_t buttons) = 0;
    virtual bool host(uint64_t sequence, const HostEvent&) = 0;
    virtual uint64_t consumed() = 0;
    virtual std::string peek(uint32_t address, uint32_t size) = 0;   // uppercase hex of guest memory, empty when unreadable
};
class TestServer {
public:
    TestServer(TestPipes&, TestInputSink&);
    // Called once per main-loop iteration with the latest published frame counter.
    struct Pump { bool capture = false; bool draws = false; bool quit = false; std::vector<uint32_t> host_keys; std::vector<std::string> options; std::vector<std::pair<float, float>> menu_clicks; };
    Pump pump(uint64_t frame, const std::string& frame_json);
    void captured(const std::string& path);           // completes the oldest pending capture
    void capture_failed(const std::string& message);
    void draws_ready(const std::string& draws_json, const std::string& projection_json);        // completes pending `draws` requests with the screen-space summary
    void release_all();                               // on disconnect/exit: release held keys/buttons, drop the schedule
private:
    struct Item { uint64_t due; uint64_t request; TestDevice device; bool press; bool last; TestInput input; };
    struct Waiting { uint64_t id; uint64_t sequence; };   // input requests whose last item was applied
    void handle(const std::string& line, uint64_t frame, const std::string& frame_json, Pump&);
    bool apply(const Item&, uint64_t frame);
    void schedule(const TestRequest&, uint64_t frame);
    void fail_request(uint64_t request, const std::string& message);
    TestPipes& pipes_; TestInputSink& sink_;
    std::deque<Item> items_;                           // ordered by due frame, insertion order within a frame
    std::deque<Waiting> waiting_;
    std::set<uint64_t> open_requests_;                 // scheduled and not yet replied
    std::deque<uint64_t> captures_, draw_requests_; bool capture_inflight_ = false;
    std::set<uint32_t> held_keys_, held_mouse_;
    float aim_x_ = 0, aim_y_ = 0; uint32_t kpad_buttons_ = 0; uint64_t kpad_owner_ = 0;
    uint64_t next_sequence_ = 1, last_frame_sent_ = 0;
};
}
