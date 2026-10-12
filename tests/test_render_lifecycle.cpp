#include "check.h"
#include "sched.h"
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
using namespace std::chrono_literals;

struct BodyFinished {
    std::promise<void>& promise;
    ~BodyFinished() { promise.set_value(); }
};
static void close_busy_guest(bool interrupts) {
    std::promise<void> entered, exited;
    auto begun = entered.get_future(), stopped = exited.get_future();
    std::atomic<bool> emergency_release{false};
    bool closed;
    {
        uc::Sched scheduler(nullptr);
        auto* guest = scheduler.create(0, 16, [&](ppc::Ctx&) {
            BodyFinished finished{exited};
            scheduler.set_interrupts(interrupts);
            entered.set_value();
            while (!emergency_release.load()) scheduler.safe_point();
        });
        std::thread coordinator([&] { scheduler.run(guest); });
        CHECK(begun.wait_for(1s) == std::future_status::ready);
        scheduler.shutdown();
        closed = stopped.wait_for(500ms) == std::future_status::ready;
        emergency_release = true; // Allow the unfixed implementation to unwind safely before reporting RED.
        coordinator.join();
    }
    CHECK(closed);
}
static void close_blocked_guest() {
    std::promise<void> entered, exited;
    auto begun = entered.get_future(), stopped = exited.get_future();
    bool resumed = false;
    uc::Sched scheduler(nullptr);
    auto* guest = scheduler.create(0, 16, [&](ppc::Ctx&) {
        BodyFinished finished{exited};
        entered.set_value();
        scheduler.block_on(0x1234);
        resumed = true;
    });
    std::thread coordinator([&] { scheduler.run(guest); });
    CHECK(begun.wait_for(1s) == std::future_status::ready);
    scheduler.shutdown(); coordinator.join();
    CHECK(stopped.wait_for(1s) == std::future_status::ready);
    CHECK(!resumed);
    auto token = std::make_shared<int>(7);
    std::weak_ptr<int> weak = token;
    scheduler.post_external([token] {});
    token.reset();
    CHECK(weak.expired()); // A completion posted after close releases its captured resources immediately.
}
int main() { close_busy_guest(true); close_busy_guest(false); close_blocked_guest(); }
