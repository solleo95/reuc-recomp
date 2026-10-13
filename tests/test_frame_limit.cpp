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
}
