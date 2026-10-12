#include "check.h"
#include "hle/save.h"
#include "hle/os.h"
#include "dispatch.h"
#include "ppc_fixture.h"
#include <cstring>

static int callbacks;
static int32_t completion;
static void done(ppc::Ctx& c) { CHECK(!uc::os_sched().interrupts_enabled()); CHECK(c.r[4]==0xBEEF); completion=int32_t(c.r[3]); ++callbacks; }
int main() {
    Fix fixture; auto& c=fixture.c; c.r[13]=0x80400000;
    uc::SaveFs fs(std::filesystem::temp_directory_path()/("uc-save-hle-"+std::to_string(GetCurrentProcessId())));
    uc::save_hle_init(fs);
    hle_NANDInit(c); CHECK(c.r[3]==0 && ppc::rd32(c,c.r[13]-25056)==2);
    CHECK(std::strcmp(reinterpret_cast<char*>(c.mem+0x004291C0),"/title/00010000/52425545/data")==0);
    ppc::wr32(c,0x8061F6C0,0x12345678); hle_NANDInit(c);
    CHECK(c.r[3]==0 && ppc::rd32(c,0x8061F6C0)==0x12345678);
    constexpr char name[]="/title/00010000/52425545/data/test.bin";
    std::memcpy(c.mem+0x1000,name,sizeof(name));
    c.r[1]=0xCC008000; hle_ISFS_GetAttrAsync(c); CHECK(int32_t(c.r[3])==-101);
    c.r[3]=0x80001000; c.r[4]=1; hle_ISFS_Open(c); CHECK(int32_t(c.r[3])==-106);
    c.r[3]=0x80001000; c.r[4]=0; c.r[5]=3; c.r[6]=0; c.r[7]=0; hle_ISFS_CreateFile(c); CHECK(c.r[3]==0);
    c.r[3]=0x80001000; c.r[4]=3; hle_ISFS_Open(c); const auto fd=c.r[3]; CHECK(int32_t(fd)>=0);
    for (uint32_t at=0x80002500;at<0x80002518;at+=4) ppc::wr32(c,at,0xDEADBEEF);
    c.r[3]=0x80001000; c.r[4]=0x80002500; c.r[5]=0x80002504;
    c.r[6]=0x80002508; c.r[7]=0x8000250C; c.r[8]=0x80002510; c.r[9]=0x80002514;
    hle_ISFS_GetAttr(c);
    CHECK(c.r[3]==0 && ppc::rd32(c,0x80002508)==0 && ppc::rd32(c,0x8000250C)==3);
    CHECK(ppc::rd32(c,0x80002510)==0 && ppc::rd32(c,0x80002514)==0);
    ppc::wr32(c,0x80002500,0xDEADBEEF);
    c.r[3]=0x80001000; c.r[9]=0x817FFFFF; hle_ISFS_GetAttr(c);
    CHECK(int32_t(c.r[3])==-101 && ppc::rd32(c,0x80002500)==0xDEADBEEF);
    ppc::wr32(c,0x80002000,0x12345678);
    c.r[3]=fd; c.r[4]=0xCC008000; c.r[5]=4; hle_ISFS_Write(c); CHECK(int32_t(c.r[3])==-101);
    CHECK(fs.stats(int(fd)).length==0);
    c.r[3]=fd; c.r[4]=0x80002000; c.r[5]=4; hle_ISFS_Write(c); CHECK(c.r[3]==4);
    c.r[3]=fd; c.r[4]=0x80002100; hle_ISFS_GetFileStats(c); CHECK(c.r[3]==0 && ppc::rd32(c,0x80002100)==4 && ppc::rd32(c,0x80002104)==4);
    CHECK(fs.create_file("/title/00010000/52425545/data/a")==0);
    constexpr char directory[]="/title/00010000/52425545/data";
    std::memcpy(c.mem+0x1100,directory,sizeof(directory));
    ppc::wr32(c,0x80002300,1);
    c.r[3]=0x80001100; c.r[4]=0x80002400; c.r[5]=0x80002300; hle_ISFS_ReadDir(c);
    CHECK(c.r[3]==0 && ppc::rd32(c,0x80002300)==1 && c.mem[0x2400]=='a' && c.mem[0x2401]==0);
    ppc::wr32(c,0x80002300,2);
    c.r[3]=0x80001100; c.r[4]=0x80002400; c.r[5]=0x80002300; hle_ISFS_ReadDir(c);
    CHECK(c.r[3]==0 && ppc::rd32(c,0x80002300)==2 && std::strcmp(reinterpret_cast<char*>(c.mem+0x2402),"test.bin")==0);
    const uc::FnEntry table[]={{0x80100000,done}}; uc::dispatch_init(table,1,0x80100000,0x80100004);
    uc::Sched sched(c.mem); uc::os_hle_init(sched,c.mem,0x817F0000);
    auto* main=sched.create(0,16,[&](ppc::Ctx& ctx) {
        ctx.r[1]=0x80003000; ppc::wr32(ctx,ctx.r[1]+8,0xBEEF);
        ctx.r[3]=0x80001000; ctx.r[4]=0x80002500; ctx.r[5]=0x80002504;
        ctx.r[6]=0x80002508; ctx.r[7]=0x8000250C; ctx.r[8]=0x80002510; ctx.r[9]=0x80002514; ctx.r[10]=0;
        ppc::wr32(ctx,0x8000250C,0xDEADBEEF); hle_ISFS_GetAttrAsync(ctx);
        CHECK(ctx.r[3]==0 && ppc::rd32(ctx,0x8000250C)==3);
        ctx.r[3]=fd; ctx.r[4]=0; ctx.r[5]=0; hle_ISFS_Seek(ctx); CHECK(ctx.r[3]==0);
        uc::os_sched().set_interrupts(false);
        ctx.r[3]=fd; ctx.r[4]=0x80002200; ctx.r[5]=4; ctx.r[6]=0x80100000; ctx.r[7]=0xBEEF;
        hle_ISFS_ReadAsync(ctx); CHECK(ctx.r[3]==0 && callbacks==0);
        uc::os_sched().set_interrupts(true);
        for (int i=0;!callbacks && i<10;++i) uc::os_sched().safe_point();
        CHECK(callbacks==1 && completion==4 && ppc::rd32(ctx,0x80002200)==0x12345678);
        ctx.r[3]=fd; hle_ISFS_Close(ctx); CHECK(ctx.r[3]==0);
    },&c);
    sched.run(main);
    uc::save_hle_shutdown();
}
