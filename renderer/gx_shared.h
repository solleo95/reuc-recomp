#pragma once
#include "block_pool.h"
#include <cstring>
#include <memory>
#include <type_traits>

namespace gx {
// An immutable block shared by many draws (a mesh's 6 draws, a display list's draws) instead of a ~2 KB copy in each. Reads go through `->`/`*`;
// a write goes through edit(), which copies first when the block is shared (copy on write). Never null: a default-constructed Shared points at one
// shared default value.
template<class T>
class Shared {
public:
    Shared() : block_(default_block()) {}
    Shared(const T& value) : block_(std::allocate_shared<const T>(PoolAllocator<T>{},value)) {}
    Shared(std::shared_ptr<const T> block) : block_(std::move(block)) {}
    Shared& operator=(const T& value) { block_ = std::allocate_shared<const T>(PoolAllocator<T>{},value); return *this; }
    const T* operator->() const { return block_.get(); }
    const T& operator*() const { return *block_; }
    operator const T&() const { return *block_; }
    const std::shared_ptr<const T>& block() const { return block_; }
    T& edit() {
        if (block_.use_count() != 1 || block_ == default_block()) block_ = std::allocate_shared<const T>(PoolAllocator<T>{},*block_);
        return const_cast<T&>(*block_);   // sole owner of a block that was created non-const above
    }
private:
    static const std::shared_ptr<const T>& default_block() { static const std::shared_ptr<const T> block = std::make_shared<const T>(); return block; }
    std::shared_ptr<const T> block_;
};
// Returns `cache` holding a block with the same bytes as `value`: the previous block when nothing changed, a new one otherwise. Bitwise, so -0.0 and
// NaN payloads are never merged with other values; padding bytes can only cause a harmless extra copy.
template<class T>
const Shared<T>& share(Shared<T>& cache, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (std::memcmp(&*cache, &value, sizeof(T)) != 0) cache = value;
    return cache;
}
}
