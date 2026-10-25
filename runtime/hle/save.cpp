#include "save.h"
#include "gx_vertices.h"
#include "guest_call.h"
#include "dispatch.h"
#include "hle/os.h"
#include <cstring>
#include <cstdio>
#include <stdexcept>

namespace {
uc::SaveFs* files; // configured before guest threads; all operations run under the scheduler baton
constexpr char kHome[]="/title/00010000/52425545/data";
gx::Memory memory(const ppc::Ctx& c) {
    return {{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
}
void check(ppc::Ctx& c,uint32_t address,size_t count) {
    if (count && !address) throw std::invalid_argument("null native file buffer");
    memory(c).read(address,count);
}
std::string path(ppc::Ctx& c,uint32_t address) {
    if (!address) throw std::invalid_argument("null native file path");
    std::string result;
    for (uint32_t i=0;i<64;++i) {
        if (uint64_t(address)+i>UINT32_MAX) throw std::invalid_argument("native file path overflows");
        const auto value=std::to_integer<char>(memory(c).read(address+i,1)[0]);
        if (!value) return result;
        result+=value;
    }
    throw std::invalid_argument("unterminated native file path");
}
template<class Function> void call(ppc::Ctx& c,Function fn) {
#ifdef UC_TRACE
    const auto caller=c.lr, arg3=c.r[3], arg4=c.r[4];
#endif
    int result=-128;
    if (files) {
        try { result=fn(); }
        catch (const gx::Error&) { result=-101; }
        catch (const std::invalid_argument&) { result=-101; }
    }
    c.r[3]=uint32_t(result);
#ifdef UC_TRACE
    std::fprintf(stderr,"[save] caller %08X args %08X %08X result %d\n",caller,arg3,arg4,result);
#endif
}
void complete(ppc::Ctx& c,void (*fn)(ppc::Ctx&),uint32_t callback,uint32_t argument) {
    if (callback && !uc::has_function(callback)) { c.r[3]=uint32_t(-101); return; }
    // shortcut: native I/O runs under the guest baton; move work to a joined worker after latency profiling (TD-039).
    fn(c);
    const uint32_t result=c.r[3];
#ifdef UC_TRACE
    std::fprintf(stderr,"[save] queue callback %08X argument %08X result %d\n",callback,argument,int32_t(result));
#endif
    auto& scheduler=uc::os_sched();
    scheduler.post_external([&scheduler,result,callback,argument] {
#ifdef UC_TRACE
        std::fprintf(stderr,"[save] deliver callback %08X argument %08X\n",callback,argument);
#endif
        if (callback) uc::call_guest(scheduler.irq_ctx(),callback,{result,argument});
#ifdef UC_TRACE
        std::fprintf(stderr,"[save] callback %08X returned\n",callback);
#endif
    },uc::Sched::EventKind::Save);
    c.r[3]=0;
}
int create(ppc::Ctx& c,bool directory) {
    if (c.r[4]>255 || c.r[5]>3 || c.r[6]>3 || c.r[7]>3) return -101;
    const auto name=path(c,c.r[3]);
    return directory ? files->create_dir(name) : files->create_file(name);
}
}
namespace uc {
void save_hle_init(SaveFs& service) { files=&service; }
void save_hle_shutdown() { files=nullptr; }
}
void hle_NANDInit(ppc::Ctx& c) {
    call(c,[&] {
        check(c,c.r[13]-25056,4); check(c,0x804291C0,64); check(c,0x8061F6C0,64);
        if (ppc::rd32(c,c.r[13]-25056)==2) return 0;
        if (auto result=files->initialize()) return result;
        for (const auto* directory:{"/title","/title/00010000","/title/00010000/52425545",kHome,"/tmp"}) {
            const auto result=files->create_dir(directory);
            if (result && result!=-105) return result;
            bool is_directory=false;
            if (files->attributes(directory,is_directory) || !is_directory) return -101;
        }
        std::memcpy(c.mem+0x004291C0,kHome,sizeof(kHome));
        std::memcpy(c.mem+0x0061F6C0,kHome,sizeof(kHome));
        gx::note_write_range(0x004291C0,sizeof(kHome));
        gx::note_write_range(0x0061F6C0,sizeof(kHome));
        ppc::wr32(c,c.r[13]-25056,2);
        std::fprintf(stderr,"[save] native NAND home %s ready\n",kHome);
        return 0;
    });
}
void hle_ISFS_OpenLib(ppc::Ctx& c) { call(c,[&] { return files->initialize(); }); }
void hle_ISFS_CreateDir(ppc::Ctx& c) { call(c,[&] { return create(c,true); }); }
void hle_ISFS_CreateFile(ppc::Ctx& c) { call(c,[&] { return create(c,false); }); }
void hle_ISFS_Open(ppc::Ctx& c) { call(c,[&] { return files->open(path(c,c.r[3]),c.r[4]); }); }
void hle_ISFS_Close(ppc::Ctx& c) { call(c,[&] { return files->close(int32_t(c.r[3])); }); }
void hle_ISFS_Read(ppc::Ctx& c) {
    call(c,[&] {
        check(c,c.r[4],c.r[5]);
        const auto count=files->read(int32_t(c.r[3]),{c.mem+(c.r[4]&0x3FFFFFFF),c.r[5]});
        if(count>0) gx::note_write_range(c.r[4],size_t(count));
        return count;
    });
}
void hle_ISFS_Write(ppc::Ctx& c) {
    call(c,[&] { check(c,c.r[4],c.r[5]); return files->write(int32_t(c.r[3]),{c.mem+(c.r[4]&0x3FFFFFFF),c.r[5]}); });
}
void hle_ISFS_Seek(ppc::Ctx& c) { call(c,[&] { return files->seek(int32_t(c.r[3]),int32_t(c.r[4]),int32_t(c.r[5])); }); }
void hle_ISFS_GetFileStats(ppc::Ctx& c) {
    call(c,[&] {
        check(c,c.r[4],8);
        const auto stats=files->stats(int32_t(c.r[3]));
        if (!stats.result) { ppc::wr32(c,c.r[4],stats.length); ppc::wr32(c,c.r[4]+4,stats.position); }
        return stats.result;
    });
}
void hle_ISFS_GetAttr(ppc::Ctx& c) {
    call(c,[&] {
        check(c,c.r[4],4); check(c,c.r[5],2);
        for (size_t i=6;i<=9;++i) check(c,c.r[i],4);
        bool directory;
        const auto result=files->attributes(path(c,c.r[3]),directory);
        // shortcut: host-user ownership maps to owner RW; persist SDK permission metadata when required (TD-040).
        if (!result) { ppc::wr32(c,c.r[4],0); ppc::wr16(c,c.r[5],0); ppc::wr32(c,c.r[6],0); ppc::wr32(c,c.r[7],3); ppc::wr32(c,c.r[8],0); ppc::wr32(c,c.r[9],0); }
        return result;
    });
}
void hle_ISFS_GetUsage(ppc::Ctx& c) {
    call(c,[&] {
        check(c,c.r[4],4); check(c,c.r[5],4);
        const auto usage=files->usage(path(c,c.r[3]));
        if (!usage.result) { ppc::wr32(c,c.r[4],usage.blocks); ppc::wr32(c,c.r[5],usage.entries); }
        return usage.result;
    });
}
void hle_ISFS_ReadDir(ppc::Ctx& c) {
    call(c,[&] {
        check(c,c.r[5],4);
        const auto directory=files->read_dir(path(c,c.r[3]));
        if (directory.result) return directory.result;
        size_t count=directory.names.size();
        if (c.r[4]) {
            const auto capacity=ppc::rd32(c,c.r[5]);
            count=std::min(count,size_t(capacity));
            check(c,c.r[4],size_t(capacity)*13);
            for (const auto& name:directory.names) if (name.size()>12) return -101;
            size_t offset=0;
            for (size_t i=0;i<count;++i) {
                const auto& name=directory.names[i];
                std::memcpy(c.mem+(c.r[4]&0x3FFFFFFF)+offset,name.c_str(),name.size()+1);
                offset+=name.size()+1;
            }
            gx::note_write_range(c.r[4],offset);
        }
        ppc::wr32(c,c.r[5],uint32_t(count)); return 0;
    });
}
void hle_ISFS_Rename(ppc::Ctx& c) { call(c,[&] { return files->rename(path(c,c.r[3]),path(c,c.r[4])); }); }
void hle_ISFS_Delete(ppc::Ctx& c) { call(c,[&] { return files->remove(path(c,c.r[3])); }); }
#define SAVE_ASYNC(Name,Callback,Argument) void hle_##Name##Async(ppc::Ctx& c) { const auto cb=Callback,arg=Argument; complete(c,hle_##Name,cb,arg); }
SAVE_ASYNC(ISFS_CreateDir,c.r[8],c.r[9])
SAVE_ASYNC(ISFS_CreateFile,c.r[8],c.r[9])
SAVE_ASYNC(ISFS_Open,c.r[5],c.r[6])
SAVE_ASYNC(ISFS_Close,c.r[4],c.r[5])
SAVE_ASYNC(ISFS_Read,c.r[6],c.r[7])
SAVE_ASYNC(ISFS_Write,c.r[6],c.r[7])
SAVE_ASYNC(ISFS_Seek,c.r[6],c.r[7])
SAVE_ASYNC(ISFS_ReadDir,c.r[6],c.r[7])
SAVE_ASYNC(ISFS_GetUsage,c.r[6],c.r[7])
SAVE_ASYNC(ISFS_Rename,c.r[5],c.r[6])
SAVE_ASYNC(ISFS_Delete,c.r[4],c.r[5])
void hle_ISFS_GetAttrAsync(ppc::Ctx& c) {
    const auto name=c.r[3];
    uint32_t arg=0;
    call(c,[&] {
        if (c.r[1]>UINT32_MAX-8) return -101;
        check(c,c.r[1]+8,4); arg=ppc::rd32(c,c.r[1]+8); return 0;
    });
    if (c.r[3]) return;
    c.r[3]=name;
    const auto cb=c.r[10];
    complete(c,hle_ISFS_GetAttr,cb,arg);
}
#undef SAVE_ASYNC
