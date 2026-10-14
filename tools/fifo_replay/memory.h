#pragma once
#include "dff.h"
#include <map>

namespace fifo_replay {
// All update types carry RAM source bytes, including the sources of TMEM loads.
class Memory {
public:
    explicit Memory(const Dff& capture);
    void advance(size_t frame,uint32_t position);
    std::span<const std::byte> read(uint32_t address,size_t size) const;
    bool recorded(uint32_t address,size_t size) const;
    std::span<const std::byte> mem1() const { return mem1_; }
    std::span<const std::byte> mem2() const { return mem2_; }
private:
    const Dff& capture_;
    std::vector<std::byte> mem1_,mem2_;
    std::map<uint32_t,uint32_t> written_;
    size_t frame_=0,next_update_=0;
    uint32_t position_=0;
    void write(const Update& update);
};
}
