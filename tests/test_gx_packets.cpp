#include "check.h"
#include "gx_packets.h"
#include <chrono>
#include <future>
#include <thread>
using namespace std::chrono_literals;

int main() {
    gx::Packets waking;
    auto waiting=std::async(std::launch::async,[&]{return waking.wait_ready(1s);});
    CHECK(waiting.wait_for(20ms)==std::future_status::timeout);
    waking.seal_batch({});
    CHECK(waiting.wait_for(1s)==std::future_status::ready && waiting.get());
    CHECK(waking.try_take());
    auto cancel_wait=std::async(std::launch::async,[&]{return waking.wait_ready(1s);});
    waking.cancel();
    CHECK(cancel_wait.wait_for(1s)==std::future_status::ready && !cancel_wait.get());
    gx::Packets packets;
    gx::FramePacket original;
    original.draws.emplace_back();
    original.draws[0].vertices.emplace_back();
    original.draws[0].vertices[0].position[0] = 7;
    CHECK(packets.seal_batch(original) == 1);
    original.draws[0].vertices[0].position[0] = 9;
    auto first = packets.try_take();
    CHECK(first && first->serial == 1 && first->draws[0].vertices[0].position[0] == 7);
    CHECK(packets.seal_batch({}) == 2);
    CHECK(!packets.complete(2)); // Queued work has not been acquired/submitted.
    std::promise<void> entered;
    auto third = std::async(std::launch::async, [&] { entered.set_value(); return packets.seal_batch({}); });
    CHECK(entered.get_future().wait_for(1s) == std::future_status::ready);
    CHECK(third.wait_for(20ms) == std::future_status::timeout);
    // Acquiring frees no capacity: both slots still represent unfinished GPU work.
    auto second = packets.try_take();
    CHECK(second && second->serial == 2 && !packets.try_take());
    CHECK(third.wait_for(20ms) == std::future_status::timeout);
    CHECK(packets.complete(1));
    CHECK(!packets.complete(1) && !packets.complete(999));
    CHECK(third.wait_for(1s) == std::future_status::ready && third.get() == 3);
    auto last = packets.try_take(); CHECK(last && last->serial == 3);
    CHECK(packets.complete(2) && packets.complete(3));
    CHECK(packets.seal_batch({}) == 4 && packets.seal_batch({}) == 5);
    std::promise<void> cancel_entered;
    auto blocked = std::async(std::launch::async, [&] {
        cancel_entered.set_value();
        try { packets.seal_batch({}); } catch (const gx::Error& e) { return e.code == gx::ErrorCode::canceled; }
        return false;
    });
    CHECK(cancel_entered.get_future().wait_for(1s) == std::future_status::ready);
    packets.cancel();
    CHECK(blocked.wait_for(1s) == std::future_status::ready && blocked.get());
    CHECK(!packets.try_take() && !packets.complete(4));
    // Two producers publish complete, independent owned packets.
    gx::Packets concurrent;
    auto produce = [&](uint32_t identity) {
        for (int i = 0; i < 8; ++i) {
            gx::FramePacket frame; frame.draws.emplace_back(); frame.draws[0].callsite = identity;
            concurrent.seal_batch(std::move(frame));
        }
    };
    std::thread a(produce, 10), b(produce, 20);
    int seen_a = 0, seen_b = 0;
    auto deadline = std::chrono::steady_clock::now() + 3s;
    uint64_t previous = 0;
    while (seen_a + seen_b < 16) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        if (auto packet = concurrent.try_take()) {
            CHECK(packet->serial == previous + 1); previous = packet->serial;
            CHECK(packet->draws.size() == 1);
            if (packet->draws[0].callsite == 10) ++seen_a;
            else { CHECK(packet->draws[0].callsite == 20); ++seen_b; }
            CHECK(concurrent.complete(packet->serial));
        } else std::this_thread::yield();
    }
    a.join(); b.join();
    CHECK(seen_a == 8 && seen_b == 8);
    return 0;
}
