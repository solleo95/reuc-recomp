#include "rollback_memory.h"
#include <atomic>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace uc {
namespace {
constexpr size_t page_size=4096;
constexpr size_t pages_per_block=64;
constexpr std::array<std::pair<uint32_t,size_t>,3> regions{{{0,24u<<20},{0x10000000,64u<<20},{0x20000000,0x10000}}};
struct Page {
    std::array<uint8_t,page_size> bytes;
    mutable std::atomic<uint64_t> hash{0};
};
struct Block { std::array<std::shared_ptr<const Page>,pages_per_block> pages; };
uint64_t page_hash(const Page& page) {
    if(const auto cached=page.hash.load(std::memory_order_relaxed)) return cached;
    uint64_t hash=0xCBF29CE484222325ull;
    for(size_t i=0;i<page_size;i+=8) {
        uint64_t word; std::memcpy(&word,page.bytes.data()+i,8);
        hash=(hash^word)*0x9E3779B97F4A7C15ull; hash^=hash>>29;
    }
    if(!hash) hash=1; // Zero marks an unhashed page.
    // Bytes are immutable: concurrent readers compute the same value.
    page.hash.store(hash,std::memory_order_relaxed);
    return hash;
}
}
struct RollbackMemory::Snapshot {
    const RollbackMemory* owner;
    std::vector<std::shared_ptr<const Block>> blocks;
    const std::shared_ptr<const Page>& page(size_t index) const { return blocks[index/pages_per_block]->pages[index%pages_per_block]; }
};
RollbackMemory::RollbackMemory(uint8_t* memory):memory_(memory) {
    if(!memory || rollback_memory_base) throw std::logic_error("rollback memory tracker already active or invalid");
    rollback_memory_base=memory; rollback_dirty.fill(0);
}
RollbackMemory::~RollbackMemory() { rollback_memory_base=nullptr; }
std::shared_ptr<const RollbackMemory::Snapshot> RollbackMemory::capture(bool verify_writes) {
    auto saved=std::make_shared<Snapshot>(); saved->owner=this;
    if(current_) saved->blocks=current_->blocks;
    else saved->blocks.resize(((((88u<<20)+0x10000)/page_size)+pages_per_block-1)/pages_per_block);
    std::shared_ptr<Block> writable;
    size_t index=0;
    for(const auto [base,size]:regions) for(size_t offset=0;offset<size;offset+=page_size,++index) {
        const auto address=base+uint32_t(offset);
        if(index%pages_per_block==0) writable.reset();
        if(current_ && !rollback_dirty[address>>12]) {
            const auto& page=current_->page(index);
            if(verify_writes && std::memcmp(page->bytes.data(),memory_+address,page_size)!=0) {
                char message[80]; std::snprintf(message,sizeof(message),"untracked rollback write in page %08X",address);
                throw std::logic_error(message);
            }
        } else {
            if(current_ && std::memcmp(current_->page(index)->bytes.data(),memory_+address,page_size)==0) continue;
            if(!writable) {
                writable=current_ ? std::make_shared<Block>(*current_->blocks[index/pages_per_block]) : std::make_shared<Block>();
                saved->blocks[index/pages_per_block]=writable;
            }
            auto page=std::make_shared_for_overwrite<Page>();
            std::memcpy(page->bytes.data(),memory_+address,page_size);
            writable->pages[index%pages_per_block]=std::move(page);
        }
    }
    current_=saved; rollback_dirty.fill(0); return saved;
}
void RollbackMemory::restore(const Snapshot& saved) {
    if(saved.owner!=this) throw std::logic_error("rollback memory snapshot belongs to another tracker");
    size_t index=0;
    for(const auto [base,size]:regions) for(size_t offset=0;offset<size;offset+=page_size,++index) {
        const auto address=base+uint32_t(offset);
        if(!current_ || saved.page(index)!=current_->page(index) || rollback_dirty[address>>12])
            std::memcpy(memory_+address,saved.page(index)->bytes.data(),page_size);
    }
    current_=std::make_shared<Snapshot>(saved); rollback_dirty.fill(0);
}
std::string RollbackMemory::difference(const Snapshot& a,const Snapshot& b) {
    size_t index=0;
    for(const auto [base,size]:regions) for(size_t offset=0;offset<size;offset+=page_size,++index) {
        if(a.blocks[index/pages_per_block]==b.blocks[index/pages_per_block] || a.page(index)==b.page(index)) continue;
        const auto& x=a.page(index)->bytes; const auto& y=b.page(index)->bytes;
        if(std::memcmp(x.data(),y.data(),page_size)==0) continue;
        size_t byte=0; while(x[byte]==y[byte]) ++byte;
        char message[120];
        std::snprintf(message,sizeof(message),"RAM %08X: expected %02X, replay %02X",base+uint32_t(offset+byte),x[byte],y[byte]);
        return message;
    }
    return {};
}
uint64_t RollbackMemory::hash(const Snapshot& saved) {
    uint64_t hash=0xCBF29CE484222325ull;
    // Only confirmed snapshots hash their new pages; shared pages retain their atomic cache.
    for(const auto& block:saved.blocks) for(const auto& page:block->pages)
        if(page) hash=(hash^page_hash(*page))*0x9E3779B97F4A7C15ull;
    return hash;
}
}
