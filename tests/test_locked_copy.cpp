#include "check.h"
#include "locked_copy.h"
#include "ppc_fixture.h"
#include <cstring>
#include <exception>
int main() {
    Fix f; auto& c=f.c;
    for (uint32_t i=0;i<4096;++i) ppc::wr8(c,0x80002000+i,uint8_t(i));
    c.r[3]=0xE0000000; c.r[4]=0x80002000; c.r[5]=0; hle_LCLoadBlocks(c);
    CHECK(std::memcmp(c.mem+0x20000000,c.mem+0x2000,4096)==0);
    c.r[3]=0x90002000; c.r[4]=0xE0000000; c.r[5]=128; hle_LCStoreBlocks(c);
    CHECK(std::memcmp(c.mem+0x10002000,c.mem+0x2000,4096)==0);
    ppc::wr8(c,0x80002000,42);
    c.r[3]=0xE0003FE0; c.r[4]=0x80002000; c.r[5]=2;
    bool rejected=false;
    try { hle_LCLoadBlocks(c); } catch (const std::exception&) { rejected=true; }
    CHECK(rejected && ppc::rd8(c,0xE0003FE0)==0);
    c.r[3]=0xE0000000; c.r[4]=0x80002000; c.r[5]=129; rejected=false;
    try { hle_LCLoadBlocks(c); } catch (const std::exception&) { rejected=true; }
    CHECK(rejected);
}
