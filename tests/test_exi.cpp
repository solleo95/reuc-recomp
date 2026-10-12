#include "check.h"
#include "ppc_fixture.h"
using namespace ppc;

void hle_EXIImm(Ctx&); void hle_EXIDma(Ctx&); void hle_EXIImmEx(Ctx&); void hle_EXISync(Ctx&);
void hle_EXILock(Ctx&); void hle_EXIGetID(Ctx&); void hle_EXIAttach(Ctx&);

int main() {
    Fix f; Ctx& c = f.c;
    for (int i = 0; i < 8; ++i) wr8(c, 0x80300000 + uint32_t(i), 0xEE);
    c.r[3] = 0; c.r[4] = 0x80300000; c.r[5] = 4; c.r[6] = 0; c.r[7] = 0; hle_EXIImm(c);          // read: no device answers, the buffer reads as zero
    CHECK(c.r[3] == 1 && rd8(c, 0x80300000) == 0 && rd8(c, 0x80300003) == 0 && rd8(c, 0x80300004) == 0xEE);
    for (int i = 0; i < 8; ++i) wr8(c, 0x80300000 + uint32_t(i), 0xEE);
    c.r[3] = 0; c.r[4] = 0x80300000; c.r[5] = 4; c.r[6] = 1; c.r[7] = 0; hle_EXIImm(c);          // write: buffer untouched
    CHECK(c.r[3] == 1 && rd8(c, 0x80300000) == 0xEE);
    c.r[3] = 0; c.r[4] = 0x80300000; c.r[5] = 8; c.r[6] = 0; c.r[7] = 0; hle_EXIDma(c);
    CHECK(c.r[3] == 1 && rd8(c, 0x80300007) == 0);
    wr8(c, 0x80300000, 0xEE);
    c.r[3] = 0; c.r[4] = 0x80300000; c.r[5] = 4; c.r[6] = 2; hle_EXIImmEx(c);                       // read/write counts as a read
    CHECK(c.r[3] == 1 && rd8(c, 0x80300000) == 0);
    c.r[3] = 0; hle_EXISync(c); CHECK(c.r[3] == 1);
    c.r[3] = 0; c.r[4] = 0; c.r[5] = 0; hle_EXILock(c); CHECK(c.r[3] == 1);
    c.r[3] = 0; c.r[4] = 0; c.r[5] = 0x80300100; hle_EXIGetID(c); CHECK(c.r[3] == 0);              // no device: no ID
    c.r[3] = 0; c.r[4] = 0; hle_EXIAttach(c); CHECK(c.r[3] == 0);
    return 0;
}
