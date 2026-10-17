#pragma once
#include <limits>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <new>
#include <type_traits>

namespace gx {
struct FrameArena {
    std::mutex mutex;
    std::pmr::monotonic_buffer_resource memory;
};

// Nodes retire together with their frame; individual deque frees need no shared pool lock.
template<class T>
class FrameAllocator {
    template<class> friend class FrameAllocator;
    std::shared_ptr<FrameArena> arena_=std::make_shared<FrameArena>();
public:
    using value_type=T;
    using propagate_on_container_move_assignment=std::true_type;
    FrameAllocator()=default;
    FrameAllocator(const FrameAllocator&)=default;
    FrameAllocator& operator=(const FrameAllocator&)=default;
    // Keep moved-from containers usable while another thread owns the published frame.
    FrameAllocator(FrameAllocator&& other) noexcept : arena_(other.arena_) {}
    FrameAllocator& operator=(FrameAllocator&& other) noexcept { arena_=other.arena_;return *this; }
    template<class U> FrameAllocator(const FrameAllocator<U>& other) noexcept : arena_(other.arena_) {}
    FrameAllocator select_on_container_copy_construction() const { return {}; }
    T* allocate(size_t n) {
        if(n>std::numeric_limits<size_t>::max()/sizeof(T)) throw std::bad_array_new_length();
        std::lock_guard lock(arena_->mutex);
        return static_cast<T*>(arena_->memory.allocate(n*sizeof(T),alignof(T)));
    }
    void deallocate(T*,size_t) noexcept {}
    template<class U> bool operator==(const FrameAllocator<U>& other) const noexcept { return arena_==other.arena_; }
};
}
