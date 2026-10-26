#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
namespace uc {
// Presentation limiter of the main loop: a finished frame waits until `interval` after the previous one was due. Unset = 240 FPS (the default of
// the game before the limit became an option), 0 = no limit.
class FrameLimit {
public:
    using Clock=std::chrono::steady_clock;
    explicit FrameLimit(std::optional<uint32_t> fps_limit=std::nullopt) { set_limit(fps_limit); }
    void set_limit(std::optional<uint32_t> fps_limit) { interval_=std::chrono::nanoseconds(fps_limit ? (*fps_limit ? 1000000000ll/ *fps_limit : 0) : 4166667); }
    Clock::duration interval() const { return interval_; }   // 0: unlimited
    bool ready(bool present,Clock::time_point now) const { return !present || interval_.count()==0 || now>=next_; }
    // The next frame is due one interval after this one was due, not after it was presented: the rate stays exact however late the loop polls;
    // a loop that fell more than one interval behind starts over from now.
    void presented(Clock::time_point now) {
        if(interval_.count()==0) return;
        next_=now>next_+interval_ ? now+interval_ : next_+interval_;
    }
private:
    std::chrono::nanoseconds interval_;
    Clock::time_point next_{};
};
}
