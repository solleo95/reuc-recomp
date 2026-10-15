#include "test_server.h"
#include "clock.h"
#include "test_state.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace uc {
namespace {
constexpr size_t kMaxOpenRequests = 256;
}
TestServer::TestServer(TestPipes& pipes, TestInputSink& sink) : pipes_(pipes), sink_(sink) {}

void TestServer::fail_request(uint64_t request, const std::string& message) {
    items_.erase(std::remove_if(items_.begin(), items_.end(), [&](const Item& i) { return i.request == request; }), items_.end());
    waiting_.erase(std::remove_if(waiting_.begin(), waiting_.end(), [&](const Waiting& w) { return w.id == request; }), waiting_.end());
    if (open_requests_.erase(request)) pipes_.send(test_reply_error(request, message), false);
}
void TestServer::schedule(const TestRequest& r, uint64_t frame) {
    const auto& in = r.input;
    const uint64_t due = std::max(in.at_frame, frame + 1);
    const bool holds = in.device == TestDevice::kpad || ((in.device == TestDevice::key || in.device == TestDevice::mouse) && in.down);
    const auto insert = [&](Item item) {
        items_.insert(std::upper_bound(items_.begin(), items_.end(), item.due, [](uint64_t d, const Item& i) { return d < i.due; }), item);
    };
    insert({due, r.id, in.device, in.device == TestDevice::kpad || in.down || in.device == TestDevice::wheel || in.device == TestDevice::motion, !holds, in});
    if (holds) insert({due + in.hold_frames, r.id, in.device, false, true, in});
    open_requests_.insert(r.id);
}
bool TestServer::apply(const Item& item, uint64_t) {
    const auto& in = item.input;
    const uint64_t sequence = next_sequence_++;
    switch (item.device) {
    case TestDevice::kpad:
        if (item.press) {
            if (!sink_.kpad(sequence, in.x, in.y, in.buttons)) return false;
            aim_x_ = in.x; aim_y_ = in.y; kpad_buttons_ = in.buttons; kpad_owner_ = item.request;
        } else if (kpad_owner_ == item.request) {   // a later kpad command owns the state now
            if (!sink_.kpad(sequence, aim_x_, aim_y_, 0)) return false;
            kpad_buttons_ = 0;
        } else --next_sequence_;
        return true;
    case TestDevice::key:
    case TestDevice::mouse: {
        const bool key = item.device == TestDevice::key;
        if (!sink_.host(sequence, {key ? HostEventKind::key : HostEventKind::mouse, in.code, item.press ? 1 : 0})) return false;
        auto& held = key ? held_keys_ : held_mouse_;
        if (item.press) held.insert(in.code); else held.erase(in.code);
        return true;
    }
    case TestDevice::wheel: return sink_.host(sequence, {HostEventKind::wheel, in.delta > 0 ? 1u : 2u, std::abs(in.delta)});
    case TestDevice::motion: return sink_.host(sequence, {HostEventKind::motion, 0, 0, in.x, in.y});
    }
    return false;
}
void TestServer::handle(const std::string& line, uint64_t frame, const std::string& frame_json, Pump& out) {
    if (line == "\x01too-long") { pipes_.send(test_reply_error(0, "command line too long"), false); return; }
    auto parsed = parse_test_request(line);
    if (auto* error = std::get_if<TestParseError>(&parsed)) { pipes_.send(test_reply_error(error->id, error->message), false); return; }
    const auto& r = std::get<TestRequest>(parsed);
    switch (r.kind) {
    case TestRequestKind::state: pipes_.send(test_reply_ok(r.id, "\"state\":" + frame_json), false); break;
    case TestRequestKind::speed:
        try { set_clock_speed(r.factor); pipes_.send(test_reply_ok(r.id, "\"factor\":" + std::to_string(r.factor)), false); }
        catch (const std::invalid_argument& e) { pipes_.send(test_reply_error(r.id, e.what()), false); }
        break;
    case TestRequestKind::capture:
        if (captures_.size() >= 16) pipes_.send(test_reply_error(r.id, "too many pending captures"), false);
        else captures_.push_back(r.id);
        break;
    case TestRequestKind::peek: {
        const auto hex = sink_.peek(r.addr, r.size);
        pipes_.send(hex.empty() ? test_reply_error(r.id, "address range not readable") : test_reply_ok(r.id, "\"hex\":\"" + hex + "\""), false);
        break;
    }
    case TestRequestKind::draws:
        if (draw_requests_.size() >= 16) pipes_.send(test_reply_error(r.id, "too many pending draws requests"), false);
        else draw_requests_.push_back(r.id);
        break;
    case TestRequestKind::quit: pipes_.send(test_reply_ok(r.id), false); out.quit = true; break;
    case TestRequestKind::input:
        if (open_requests_.size() >= kMaxOpenRequests) pipes_.send(test_reply_error(r.id, "too many queued commands"), false);
        else if (open_requests_.count(r.id)) pipes_.send(test_reply_error(r.id, "duplicate id"), false);
        else schedule(r, frame);
        break;
    }
}
TestServer::Pump TestServer::pump(uint64_t frame, const std::string& frame_json) {
    Pump out;
    for (const auto& line : pipes_.take_lines()) handle(line, frame, frame_json, out);
    while (!items_.empty() && items_.front().due <= frame) {
        const Item item = items_.front(); items_.pop_front();
        if (!apply(item, frame)) { fail_request(item.request, "input rejected by the game"); continue; }
        if (item.last) waiting_.push_back({item.request, next_sequence_ - 1});
    }
    const auto consumed = sink_.consumed();
    for (auto it = waiting_.begin(); it != waiting_.end();) {
        if (consumed >= it->sequence) {
            pipes_.send(test_reply_ok(it->id, "\"consumed_frame\":" + std::to_string(frame)), false);
            open_requests_.erase(it->id); it = waiting_.erase(it);
        } else ++it;
    }
    for (const auto& a : test_state_take_actions())
        pipes_.send("{\"type\":\"action\",\"name\":" + json_string(a.name) + ",\"accepted\":" + (a.accepted ? "true" : "false") + ",\"frame\":" + std::to_string(a.frame) + "}", false);
    if (frame != last_frame_sent_) { last_frame_sent_ = frame; pipes_.send(frame_json, true); }
    out.draws = !draw_requests_.empty();
    if (!captures_.empty() && !capture_inflight_) { capture_inflight_ = true; out.capture = true; }
    return out;
}
void TestServer::captured(const std::string& path) {
    capture_inflight_ = false;
    if (captures_.empty()) return;
    pipes_.send(test_reply_ok(captures_.front(), "\"path\":" + json_string(path)), false); captures_.pop_front();
}
void TestServer::draws_ready(const std::string& draws_json, const std::string& projection_json) {
    for (auto id : draw_requests_) pipes_.send(test_reply_ok(id, "\"draws\":" + draws_json + ",\"projection\":" + projection_json), false);
    draw_requests_.clear();
}
void TestServer::capture_failed(const std::string& message) {
    capture_inflight_ = false;
    if (captures_.empty()) return;
    pipes_.send(test_reply_error(captures_.front(), message), false); captures_.pop_front();
}
void TestServer::release_all() {
    for (auto code : held_keys_) sink_.host(next_sequence_++, {HostEventKind::key, code, 0});
    for (auto code : held_mouse_) sink_.host(next_sequence_++, {HostEventKind::mouse, code, 0});
    if (kpad_buttons_) sink_.kpad(next_sequence_++, aim_x_, aim_y_, 0);
    held_keys_.clear(); held_mouse_.clear(); kpad_buttons_ = 0;
    items_.clear(); waiting_.clear(); open_requests_.clear();
}
}  // namespace uc
