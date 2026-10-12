// IOS call interface (the SDK's IOS_* functions) replaced at call level; no IPC register or request ring is driven.
// No IOS device exists yet: opens fail with "not found" and every other call reports an invalid descriptor (tech-debt TD-012).
#include "fatal.h"
#include "guest_call.h"
#include "hle/os.h"
#include "ppc_rt.h"
#include <cstdio>
#include <string>
using namespace ppc;

namespace {
constexpr int32_t kEInval = -4, kENoEnt = -6;
void finish_async(Ctx& c, int32_t result, uint32_t cb, uint32_t arg) {
    uc::Sched& s = uc::os_sched();
    s.post_external([&s, result, cb, arg] {                    // interrupt context, like the real IPC reply
        if (cb) uc::call_guest(s.irq_ctx(), cb, {uint32_t(result), arg});
    });
    c.r[3] = 0;
}
std::string cstr(Ctx& c, uint32_t a) { std::string s; for (uint32_t i = 0; i < 128; ++i) { char ch = char(rd8(c, a + i)); if (!ch) break; s += ch; } return s; }
void log_call(Ctx& c, const char* what, uint32_t a) { std::fprintf(stderr, "[ios] %s %s\n", what, cstr(c, a).c_str()); }
}  // namespace

void hle_IOS_Open(Ctx& c) { log_call(c, "open", c.r[3]); c.r[3] = uint32_t(kENoEnt); }
void hle_IOS_OpenAsync(Ctx& c) { log_call(c, "open", c.r[3]); finish_async(c, kENoEnt, c.r[5], c.r[6]); }
void hle_IOS_Close(Ctx& c) { c.r[3] = uint32_t(kEInval); }
void hle_IOS_CloseAsync(Ctx& c) { finish_async(c, kEInval, c.r[4], c.r[5]); }
void hle_IOS_Read(Ctx& c) { c.r[3] = uint32_t(kEInval); }
void hle_IOS_ReadAsync(Ctx& c) { finish_async(c, kEInval, c.r[6], c.r[7]); }
void hle_IOS_Write(Ctx& c) { c.r[3] = uint32_t(kEInval); }
void hle_IOS_WriteAsync(Ctx& c) { finish_async(c, kEInval, c.r[6], c.r[7]); }
void hle_IOS_Seek(Ctx& c) { c.r[3] = uint32_t(kEInval); }
void hle_IOS_SeekAsync(Ctx& c) { finish_async(c, kEInval, c.r[6], c.r[7]); }
void hle_IOS_Ioctl(Ctx& c) { c.r[3] = uint32_t(kEInval); }
void hle_IOS_IoctlAsync(Ctx& c) { finish_async(c, kEInval, c.r[9], c.r[10]); }
void hle_IOS_Ioctlv(Ctx& c) { c.r[3] = uint32_t(kEInval); }
void hle_IOS_IoctlvAsync(Ctx& c) { finish_async(c, kEInval, c.r[8], c.r[9]); }
