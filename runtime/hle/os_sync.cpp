#include "hle/os.h"
#include <deque>
#include <map>

using namespace uc;

namespace {
struct MutexState { Thread* owner = nullptr; int count = 0; };
struct QueueState { std::deque<uint32_t> msgs; uint32_t cap = 0; };
std::map<uint32_t, MutexState> g_mutex;
std::map<uint32_t, QueueState> g_queue;
constexpr uint32_t recv_key(uint32_t q) { return q; }
constexpr uint32_t send_key(uint32_t q) { return q + 1; }   // unaligned, so it never equals an object address
}  // namespace

// Mutex and queue state lives on the host side keyed by the guest object address; the guest fields are never read.
// tech-debt TD-009: no priority inheritance.
void hle_OSInitMutex(ppc::Ctx& c) { g_mutex[c.r[3]] = {}; }
void hle_OSLockMutex(ppc::Ctx& c) {
    uint32_t key = c.r[3];
    Thread* me = os_sched().current();
    for (;;) {
        MutexState& m = g_mutex[key];
        if (!m.owner || m.owner == me) { m.owner = me; ++m.count; return; }
        os_sched().block_on(key);
    }
}
void hle_OSUnlockMutex(ppc::Ctx& c) {
    uint32_t key = c.r[3];
    MutexState& m = g_mutex[key];
    if (m.owner != os_sched().current()) return;
    if (--m.count == 0) { m.owner = nullptr; os_sched().wake_all(key); }   // all waiters retry: a suspended one must not hold up the rest
}

void hle_OSInitMessageQueue(ppc::Ctx& c) { g_queue[c.r[3]] = {{}, c.r[5]}; }   // (queue, array, count)
static void send(ppc::Ctx& c, bool front) {                                     // (queue, msg, flags); OS_MESSAGE_BLOCK = 1
    uint32_t q = c.r[3], msg = c.r[4], flags = c.r[5];
    for (;;) {
        QueueState& s = g_queue[q];
        if (s.msgs.size() < s.cap) {
            if (front) s.msgs.push_front(msg); else s.msgs.push_back(msg);
            os_sched().wake_all(recv_key(q));
            c.r[3] = 1;
            return;
        }
        if (!(flags & 1)) { c.r[3] = 0; return; }
        os_sched().block_on(send_key(q));
    }
}
void hle_OSSendMessage(ppc::Ctx& c) { send(c, false); }
void hle_OSJamMessage(ppc::Ctx& c) { send(c, true); }
void hle_OSReceiveMessage(ppc::Ctx& c) {                                        // (queue, void** msg, flags)
    uint32_t q = c.r[3], out = c.r[4], flags = c.r[5];
    for (;;) {
        QueueState& s = g_queue[q];
        if (!s.msgs.empty()) {
            uint32_t m = s.msgs.front();
            s.msgs.pop_front();
            if (out) ppc::wr32(c, out, m);
            os_sched().wake_all(send_key(q));
            c.r[3] = 1;
            return;
        }
        if (!(flags & 1)) { c.r[3] = 0; return; }
        os_sched().block_on(recv_key(q));
    }
}
