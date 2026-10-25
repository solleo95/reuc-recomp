#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace uc {
// Guest baton owns this tracker, including native HLE writes. Independent of GX cache epochs.
inline uint8_t* rollback_memory_base=nullptr;
inline std::array<uint8_t,1u<<18> rollback_dirty{};
inline void rollback_note_write(uint32_t address) {
    if(rollback_memory_base) rollback_dirty[(address&0x3FFFFFFFu)>>12]=1;
}
inline void rollback_note_range(uint32_t address,size_t size) {
    if(!rollback_memory_base || !size) return;
    const uint64_t first=(address&0x3FFFFFFFu)>>12,last=((address&0x3FFFFFFFu)+uint64_t(size)-1)>>12;
    for(uint64_t page=first;page<=last && page<rollback_dirty.size();++page) rollback_dirty[page]=1;
}
inline void rollback_note_pointer(const uint8_t* pointer,size_t size) {
    if(rollback_memory_base) rollback_note_range(uint32_t(uintptr_t(pointer)-uintptr_t(rollback_memory_base)),size);
}
class RollbackMemory {
public:
    struct Snapshot;
    explicit RollbackMemory(uint8_t* memory);
    ~RollbackMemory();
    RollbackMemory(const RollbackMemory&)=delete;
    RollbackMemory& operator=(const RollbackMemory&)=delete;
    std::shared_ptr<const Snapshot> capture(bool verify_writes=false);
    void restore(const Snapshot&);
    static std::string difference(const Snapshot&,const Snapshot&);
    static uint64_t hash(const Snapshot&);
private:
    uint8_t* memory_;
    std::shared_ptr<const Snapshot> current_;
};
}
