#include "entry_probe.h"
#include "gx_vertices.h"
#include <algorithm>
#include <array>
#include <cstdio>
namespace uc {
namespace {
bool enabled;
EntryProbeStats stats;
std::array<uint64_t,64> seen;
size_t seen_count;
PictureProbeStats picture_stats;
std::array<uint64_t,128> pictures_seen;
size_t picture_count;
std::array<uint32_t,128> layout_resources;
size_t layout_count;
uint32_t word(std::span<const std::byte> bytes,size_t offset) {
    uint32_t result=0;for(size_t i=0;i<4;++i)result=(result<<8)|std::to_integer<uint8_t>(bytes[offset+i]);return result;
}
}
void entry_probe_init(bool active) {enabled=active;stats={};seen_count=0;picture_stats={};picture_count=0;layout_count=0;}
EntryProbeStats entry_probe_stats() {return stats;}
PictureProbeStats picture_probe_stats() {return picture_stats;}
void layout_entry_probe(const ppc::Ctx& c) {
    if(!enabled || !c.mem || layout_count==layout_resources.size() || std::find(layout_resources.begin(),layout_resources.begin()+layout_count,c.r[4])!=layout_resources.begin()+layout_count)return;
    try {
        gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
        const auto header=memory.read(c.r[4],16);
        if(word(header,0)!=0x524C5954)return;
        layout_resources[layout_count++]=c.r[4];
        std::fprintf(stderr,"[layout-entry] object %08X resource %08X size %u accessor %08X caller %08X",c.r[3],c.r[4],word(header,8),c.r[5],c.lr);
        for(size_t i=14;i<32;++i) {
            if(c.r[i]<0x80000000)continue;
            try {
                const auto bytes=memory.read(c.r[i],64);
                const auto end=std::find(bytes.begin(),bytes.end(),std::byte{});
                if(end==bytes.end())continue;
                std::string name(reinterpret_cast<const char*>(bytes.data()),size_t(end-bytes.begin()));
                if(name.ends_with(".brlyt"))std::fprintf(stderr," name-r%zu %s",i,name.c_str());
            } catch(const gx::Error&) {continue;} // A register may hold a scalar rather than a readable filename.
        }
        std::fprintf(stderr,"\n");
    } catch(const gx::Error& error) {std::fprintf(stderr,"[layout-entry] unreadable resource header: %s\n",error.what());}
}
void picture_entry_probe(const ppc::Ctx& c,uint32_t entry) {
    if(!enabled || (entry!=0x8038BC60 && entry!=0x803CFA84))return;
    ++picture_stats.calls;
    const auto key=(uint64_t(entry)<<32)|c.r[3];
    if(std::find(pictures_seen.begin(),pictures_seen.begin()+picture_count,key)!=pictures_seen.begin()+picture_count || picture_count==pictures_seen.size())return;
    if(!c.mem || !c.r[3]) {++picture_stats.invalid;return;}
    try {
        gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
        const auto bytes=memory.read(c.r[3],0xEC);
        const auto name=entry==0x8038BC60 ? 0xB8 : 0xB4;
        const auto alpha=std::to_integer<uint8_t>(bytes[entry==0x8038BC60 ? 0xB5 : 0xCE]);
        pictures_seen[picture_count++]=key;++picture_stats.records;
        std::fprintf(stderr,"[picture-entry] entry %08X object %08X name %.16s alpha %u material %08X caller %08X\n",entry,c.r[3],reinterpret_cast<const char*>(bytes.data()+name),alpha,word(bytes,0x28),c.lr);
    } catch(const gx::Error& error) {
        ++picture_stats.invalid;
        if(picture_stats.invalid<=4)std::fprintf(stderr,"[picture-entry] unreadable span: %s\n",error.what());
    }
}
void game_event_probe(const ppc::Ctx& c) {
    if(!enabled)return;
    ++stats.events;
    if(!c.mem || !c.r[3] || !c.r[4]) {++stats.invalid;return;}
    try {
        gx::Memory memory{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};
        const auto actor=memory.read(c.r[3],0x7A0),event=memory.read(c.r[4],4);
        const auto vtable=word(event,0),getter=word(memory.read(vtable,16),12),mode=word(actor,0x630);
        const auto key=(uint64_t(vtable)<<32)|mode;
        if(std::find(seen.begin(),seen.begin()+seen_count,key)!=seen.begin()+seen_count || seen_count==seen.size())return;
        seen[seen_count++]=key;++stats.records;
        std::fprintf(stderr,"[player-event] actor %08X event %08X vtable %08X getter %08X mode630 %u player790 %u flags798 %08X caller %08X\n",
            c.r[3],c.r[4],vtable,getter,mode,word(actor,0x790),word(actor,0x798),c.lr);
    } catch(const gx::Error& error) {
        ++stats.invalid;
        if(stats.invalid<=4)std::fprintf(stderr,"[player-event] unreadable actor/event span: %s\n",error.what());
    }
}
}
