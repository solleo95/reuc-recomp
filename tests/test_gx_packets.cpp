#include "check.h"
#include "gx_packets.h"
#include <chrono>
#include <future>
#include <thread>
using namespace std::chrono_literals;
static_assert(gx::PoolAllocator<gx::DrawPacket>::pooled,"Draw nodes must stay pooled when DrawPacket changes size");

int main() {
    gx::PoolAllocator<gx::DrawPacket> allocator;
    auto* node=allocator.allocate(1);
    std::thread recycle([&]{allocator.deallocate(node,1);});
    recycle.join();
    auto* reused=allocator.allocate(1);
    CHECK(reused==node);
    allocator.deallocate(reused,1);
    gx::Shared<gx::DrawUniform> uniforms;
    uniforms.edit().pixel[0]=17;
    auto edited=std::async(std::launch::async,[view=uniforms]() mutable {
        view.edit().pixel[0]=29;
        return view;
    }).get();
    CHECK(uniforms->pixel[0]==17 && edited->pixel[0]==29);
    CHECK(gx::Shared<gx::DrawUniform>{}->pixel[0]==gx::DrawUniform{}.pixel[0]);
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
    // Packets that do not present are bounded by the queue window, not by the frames in flight.
    gx::Packets window;
    gx::FramePacket pass; pass.present = false;
    for (size_t i = 0; i < gx::kMaxOutstandingPackets; ++i) CHECK(window.seal_batch(pass) == i + 1);
    auto overflow = std::async(std::launch::async, [&] { return window.seal_batch(pass); });
    CHECK(overflow.wait_for(20ms) == std::future_status::timeout);
    auto head = window.try_take(); CHECK(head && window.complete(head->serial));
    CHECK(overflow.wait_for(1s) == std::future_status::ready);
    // A presenting packet also waits for the frame in flight limit while earlier passes keep flowing.
    gx::Packets frames;
    CHECK(frames.seal_batch({}) == 1 && frames.seal_batch({}) == 2);
    auto third_frame = std::async(std::launch::async, [&] { return frames.seal_batch({}); });
    CHECK(third_frame.wait_for(20ms) == std::future_status::timeout);
    CHECK(frames.seal_batch(pass) == 3);
    auto one = frames.try_take(); CHECK(one && one->serial == 1 && frames.complete(1));
    CHECK(third_frame.wait_for(1s) == std::future_status::ready && third_frame.get() == 4);
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
