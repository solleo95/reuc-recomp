#include "locked_copy.h"
#include "gx_vertices.h"
#include <cstring>
#include <stdexcept>
namespace {
void copy(ppc::Ctx& c,bool load) {
    const auto destination=c.r[3],source=c.r[4],blocks=c.r[5];
    if (blocks>128) throw std::invalid_argument("LC transfer exceeds128blocks");
    const auto locked=load ? destination : source;
    if (locked<0xE0000000 || locked>=0xE0004000) throw std::invalid_argument("LC transfer requires locked-cache address");
    const size_t bytes=(blocks ? blocks : 128)*32;
    const gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x20000000),0x4000}};
    memory.read(source,bytes); memory.read(destination,bytes);
    std::memmove(c.mem+(destination&0x3FFFFFFF),c.mem+(source&0x3FFFFFFF),bytes);
}
}
void hle_LCLoadBlocks(ppc::Ctx& c) { copy(c,true); }
void hle_LCStoreBlocks(ppc::Ctx& c) { copy(c,false); }
