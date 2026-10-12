#include "check.h"
#include "dispatch.h"
#include "hle/os.h"
#include "ppc_fixture.h"
#include "sched.h"
#include <string>
#include <vector>
using namespace ppc;

void hle_IOS_Open(Ctx&); void hle_IOS_OpenAsync(Ctx&); void hle_IOS_Close(Ctx&); void hle_IOS_Read(Ctx&);
void hle_IOS_Ioctl(Ctx&); void hle_IOS_Ioctlv(Ctx&); void hle_IOS_IoctlAsync(Ctx&);

static std::vector<std::string> g_log;
static void guest_cb(Ctx& c) { g_log.push_back("cb(" + std::to_string(int(c.r[3])) + "," + std::to_string(c.r[4]) + ")"); uc::os_sched().wake_all(0x6000); }

int main() {
    Fix f; Ctx& c = f.c;
    uc::Sched s(c.mem);
    static const uc::FnEntry table[] = {{0x80100000, guest_cb}};
    uc::dispatch_init(table, 1, 0x80100000, 0x80100004);
    uc::os_hle_init(s, c.mem, 0x817F0000);
    s.irq_ctx().r[1] = 0x817EFF00;
    uc::Thread* m = s.create(0, 16, [&](Ctx& t) {
        const char* path = "/dev/es";
        for (size_t i = 0; i <= 7; ++i) wr8(t, 0x80360000 + uint32_t(i), uint32_t(i < 7 ? path[i] : 0));
        t.r[3] = 0x80360000; t.r[4] = 0; hle_IOS_Open(t);
        CHECK(int(t.r[3]) == -6);                                   // no IOS device exists yet: opens fail with "not found"
        t.r[3] = 3; hle_IOS_Close(t); CHECK(int(t.r[3]) == -4);     // so every other call sees an invalid descriptor
        t.r[3] = 3; t.r[4] = 0x80361000; t.r[5] = 16; hle_IOS_Read(t); CHECK(int(t.r[3]) == -4);
        t.r[3] = 3; hle_IOS_Ioctl(t); CHECK(int(t.r[3]) == -4);
        t.r[3] = 3; hle_IOS_Ioctlv(t); CHECK(int(t.r[3]) == -4);
        t.r[3] = 0x80360000; t.r[4] = 0; t.r[5] = 0x80100000; t.r[6] = 77; hle_IOS_OpenAsync(t);
        CHECK(t.r[3] == 0);                                         // accepted; the result arrives in the callback
        s.block_on(0x6000);
        CHECK(g_log.size() == 1 && g_log[0] == "cb(-6,77)");
        t.r[3] = 3; t.r[4] = 1; t.r[5] = 0; t.r[6] = 0; t.r[7] = 0; t.r[9] = 0x80100000; t.r[10] = 5; hle_IOS_IoctlAsync(t);
        s.block_on(0x6000);
        CHECK(g_log.size() == 2 && g_log[1] == "cb(-4,5)");
    });
    s.run(m);
    return 0;
}
