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
    struct State { std::mutex mutex; std::vector<void*> blocks; };
    // Never destroyed: blocks are also returned by destructors of globals (and by the render thread) while the process exits.
    static State& state() { static State* state = new State; return *state; }
    static void* get() {
        auto& pool = state();
        { std::lock_guard lock(pool.mutex); if (!pool.blocks.empty()) { void* block = pool.blocks.back(); pool.blocks.pop_back(); return block; } }
        return ::operator new(Size);
    }
    static void put(void* block) {
        auto& pool = state();
        { std::lock_guard lock(pool.mutex); if (pool.blocks.size() < 4096) { pool.blocks.push_back(block); return; } }
        ::operator delete(block);
    }
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
