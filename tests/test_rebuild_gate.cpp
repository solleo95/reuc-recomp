#include "check.h"
#include "rebuild_gate.h"
int main() {
    using namespace std::chrono_literals;
    const auto t0 = std::chrono::steady_clock::time_point{} + 10s;
    CHECK(uc::rebuild_due(true, false, t0, t0));                      // no swapchain yet: rebuild at once
    CHECK(!uc::rebuild_due(false, true, t0 + 1s, t0));                // size unchanged: never
    CHECK(!uc::rebuild_due(true, true, t0 + 50ms, t0));               // a resize storm: wait for the interval
    CHECK(uc::rebuild_due(true, true, t0 + uc::kRebuildInterval, t0)); // then rebuild once
    CHECK(!uc::rebuild_due(false, false, t0 + 1s, t0));               // nothing requested
}
