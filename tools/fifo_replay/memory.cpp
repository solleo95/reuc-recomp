#include "memory.h"
#include <algorithm>

namespace fifo_replay {
namespace {
uint32_t physical(uint32_t address) {
    auto alias=address>>28;
    if(alias!=0 && alias!=1 && alias!=8 && alias!=9 && alias!=12 && alias!=13)
        throw std::runtime_error("DFF: unsupported memory address");
    return address&0x3FFFFFFF;
}
template<class T> auto region(T& mem1,T& mem2,uint32_t address,size_t size) {
    auto& bank=address<0x10000000?mem1:mem2;
    size_t offset=address<0x10000000?address:address-0x10000000;
    if(offset>bank.size() || size>bank.size()-offset)
        throw std::runtime_error("DFF: memory range outside RAM");
    return std::span(bank).subspan(offset,size);
}
}
Memory::Memory(const Dff& capture):capture_(capture),mem1_(capture.mem1_size),mem2_(capture.mem2_size) {}
void Memory::write(const Update& update) {
    uint32_t address=physical(update.address);
    auto dst=region(mem1_,mem2_,address,update.data.size);
    auto src=capture_.data(update.data);
    std::copy(src.begin(),src.end(),dst.begin());
    if(src.empty()) return;
    uint32_t begin=address,end=address+update.data.size;
    auto it=written_.lower_bound(begin);
    if(it!=written_.begin()) {
        auto prev=std::prev(it);
        if(prev->second>=begin) it=prev;
    }
    while(it!=written_.end() && it->first<=end) {
        begin=std::min(begin,it->first); end=std::max(end,it->second);
        it=written_.erase(it);
    }
    written_.emplace(begin,end);
}
void Memory::advance(size_t frame,uint32_t position) {
    if(frame>=capture_.frames.size()) throw std::runtime_error("DFF: frame outside capture");
    if(position>capture_.frames[frame].fifo.size) throw std::runtime_error("DFF: FIFO position outside frame");
    if(frame!=frame_) {
        if(frame!=frame_+1 || position_!=capture_.frames[frame_].fifo.size ||
           next_update_!=capture_.frames[frame_].updates.size())
            throw std::runtime_error("DFF: replay must process preceding frames");
        frame_=frame; position_=0; next_update_=0;
    }
    const auto& f=capture_.frames[frame];
    if(position<position_ || position>f.fifo.size) throw std::runtime_error("DFF: non-monotonic FIFO position");
    while(next_update_<f.updates.size() && f.updates[next_update_].position<=position) {
        write(f.updates[next_update_]);
        ++next_update_;
    }
    position_=position;
}
std::span<const std::byte> Memory::read(uint32_t address,size_t size) const {
    address=physical(address);
    auto bytes=region(mem1_,mem2_,address,size);
    if(size==0) return bytes;
    auto it=written_.upper_bound(address);
    if(it==written_.begin()) throw std::runtime_error("DFF: unrecorded memory read");
    --it;
    if(uint64_t(address)+size>it->second) throw std::runtime_error("DFF: partially recorded memory read");
    return bytes;
}
bool Memory::recorded(uint32_t address,size_t size) const {
    address=physical(address);(void)region(mem1_,mem2_,address,size);
    if(!size)return true;
    auto it=written_.upper_bound(address);
    return it!=written_.begin() && uint64_t(address)+size<=std::prev(it)->second;
}
}
