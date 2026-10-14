#include "dff.h"
#include <fstream>
#include <string>

namespace fifo_replay {
namespace {
// shortcut: v5/v6 captures up to 512 MiB; extend with fixtures when older/larger captures are needed.
constexpr size_t max_bytes=512ull*1024*1024;
[[noreturn]] void fail(const char* reason) { throw std::runtime_error(std::string("DFF: ")+reason); }
void bounds(size_t size,uint64_t offset,uint64_t count) {
    if(offset>size || count>size-offset) fail("section outside file");
}
uint64_t read(std::span<const std::byte> b,size_t p,size_t n=4) {
    bounds(b.size(),p,n);
    uint64_t v=0;
    for(size_t i=0;i<n;++i) v|=uint64_t(std::to_integer<uint8_t>(b[p+i]))<<(8*i);
    return v;
}
template<size_t N> void bank(std::span<const std::byte> b,size_t p,std::array<uint32_t,N>& out) {
    auto offset=read(b,p,8),count=read(b,p+8);
    if(count>N) fail("oversized register bank");
    bounds(b.size(),offset,count*4);
    for(size_t i=0;i<count;++i) out[i]=uint32_t(read(b,size_t(offset)+i*4));
}
}
std::span<const std::byte> Dff::data(Range r) const {
    bounds(bytes.size(),r.offset,r.size);
    return std::span<const std::byte>(bytes).subspan(size_t(r.offset),r.size);
}
Dff parse_dff(std::vector<std::byte> bytes) {
    if(bytes.size()<128 || bytes.size()>max_bytes) fail("invalid file size");
    Dff d; d.bytes=std::move(bytes);
    const std::span<const std::byte> b=d.bytes;
    if(read(b,0)!=0x0D01F1F0) fail("invalid magic");
    auto version=read(b,4);
    if((version!=5 && version!=6) || read(b,8)>version) fail("unsupported version (expected v5/v6)");
    bank(b,12,d.bp); bank(b,24,d.cp); bank(b,36,d.xf); bank(b,48,d.xf_registers);
    d.wii=(read(b,72)&1)!=0;
    d.tmem={read(b,76,8),uint32_t(read(b,84))};
    if(d.tmem.size>1024*1024) fail("oversized texture memory");
    (void)d.data(d.tmem);
    d.mem1_size=uint32_t(read(b,88)); d.mem2_size=uint32_t(read(b,92));
    if(d.mem1_size>64*1024*1024 || d.mem2_size>128*1024*1024) fail("unsupported RAM size");
    if(version>=6)
        for(size_t i=0;i<d.game_id.size();++i) d.game_id[i]=char(std::to_integer<uint8_t>(b[96+i]));
    auto frames_offset=read(b,60,8),count=read(b,68);
    bounds(b.size(),frames_offset,count*64);
    if(count>100000) fail("too many frames");
    d.frames.reserve(size_t(count));
    size_t update_count=0;
    for(size_t i=0;i<count;++i) {
        size_t p=size_t(frames_offset)+i*64;
        Frame f;
        f.fifo={read(b,p,8),uint32_t(read(b,p+8))};
        (void)d.data(f.fifo);
        f.fifo_start=uint32_t(read(b,p+12)); f.fifo_end=uint32_t(read(b,p+16));
        auto offset=read(b,p+20,8),updates=read(b,p+28);
        bounds(b.size(),offset,updates*24);
        if(updates>1000000-update_count) fail("too many memory updates");
        update_count+=size_t(updates);
        f.updates.reserve(size_t(updates));
        uint32_t previous=0;
        for(size_t j=0;j<updates;++j) {
            size_t q=size_t(offset)+j*24;
            Update u;
            u.position=uint32_t(read(b,q)); u.address=uint32_t(read(b,q+4));
            u.data={read(b,q+8,8),uint32_t(read(b,q+16))};
            u.type=uint8_t(read(b,q+20,1));
            if(u.position<previous || u.position>f.fifo.size) fail("invalid memory update position");
            if(u.type!=1 && u.type!=2 && u.type!=4 && u.type!=8) fail("unknown memory update type");
            (void)d.data(u.data);
            previous=u.position;
            f.updates.push_back(u);
        }
        d.frames.push_back(std::move(f));
    }
    return d;
}
Dff load_dff(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file) fail("cannot open file");
    auto size=file.tellg();
    if(size<128 || size>std::streamoff(max_bytes)) fail("invalid file size");
    std::vector<std::byte> bytes(static_cast<size_t>(size));
    file.seekg(0);
    if(!file.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size()))) fail("cannot read file");
    return parse_dff(std::move(bytes));
}
}
