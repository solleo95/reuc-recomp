#pragma once
#include <cstddef>
#include <mutex>
#include <new>
#include <vector>

namespace gx {
// Recycles single large blocks (a deque node holding one DrawPacket, ~6 KB): the guest thread allocates them, the render thread frees them.
// The default heap hands such blocks back to the OS and faults fresh pages in every frame. Free blocks are capped; the rest go back to the heap.
template<size_t Size>
struct BlockPool {
    static void* get() {
        { std::lock_guard lock(mutex); if (!blocks.empty()) { void* block = blocks.back(); blocks.pop_back(); return block; } }
        return ::operator new(Size);
    }
    static void put(void* block) {
        { std::lock_guard lock(mutex); if (blocks.size() < 4096) { blocks.push_back(block); return; } }
        ::operator delete(block);
    }
    static inline std::mutex mutex;
    static inline std::vector<void*> blocks;
};
template<class T>
struct PoolAllocator {
    using value_type = T;
    static constexpr bool pooled = sizeof(T) >= 1024;
    PoolAllocator() = default;
    template<class U> PoolAllocator(const PoolAllocator<U>&) {}
    T* allocate(size_t n) { return static_cast<T*>(pooled && n == 1 ? BlockPool<sizeof(T)>::get() : ::operator new(n * sizeof(T))); }
    void deallocate(T* block, size_t n) { if (pooled && n == 1) BlockPool<sizeof(T)>::put(block); else ::operator delete(block); }
    template<class U> bool operator==(const PoolAllocator<U>&) const { return true; }
};
}
