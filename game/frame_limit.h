#pragma once
#include <chrono>
namespace uc {
class FrameLimit {
public:
    using Clock=std::chrono::steady_clock;
    bool ready(bool present,Clock::time_point now) const { return !present || now>=next_; }
    void presented(Clock::time_point now) { next_=now+std::chrono::nanoseconds(4166667); }
private:
    Clock::time_point next_{};
};
}
