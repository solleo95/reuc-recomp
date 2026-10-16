#include "check.h"
#include "frame_limit.h"
int main() {
    using namespace std::chrono;
    const steady_clock::time_point start{};
    uc::FrameLimit limit;
    CHECK(limit.ready(true,start));
    limit.presented(start);
    CHECK(!limit.ready(true,start+nanoseconds(4166666)));
    CHECK(limit.ready(true,start+nanoseconds(4166667)));
    CHECK(limit.ready(false,start+nanoseconds(1)));
    const auto late=start+seconds(1);
    CHECK(limit.ready(true,late));
    limit.presented(late);
    CHECK(!limit.ready(true,late+nanoseconds(1)));
    CHECK(limit.ready(true,late+nanoseconds(4166667)));
    // configurable limit (launcher "Frame rate limit")
    const auto t0=steady_clock::time_point{}+seconds(10);
    uc::FrameLimit sixty(60);
    sixty.presented(t0);
    CHECK(!sixty.ready(true,t0+milliseconds(16)) && sixty.ready(true,t0+microseconds(16700)));
    // the rate stays exact when the loop polls late: due times advance by whole intervals
    uc::FrameLimit exact(100);
    exact.presented(t0);                      // next due t0 + 10 ms
    exact.presented(t0+milliseconds(14));     // presented 4 ms late: the following frame is still due at t0 + 20 ms
    CHECK(!exact.ready(true,t0+microseconds(19900)) && exact.ready(true,t0+milliseconds(20)));
    // a loop that stalled starts over from now instead of presenting a burst
    exact.presented(t0+seconds(5));
    CHECK(!exact.ready(true,t0+seconds(5)+milliseconds(5)) && exact.ready(true,t0+seconds(5)+milliseconds(10)));
    uc::FrameLimit unlimited(0);              // 0 = no limit
    unlimited.presented(t0);
    CHECK(unlimited.ready(true,t0));
}
