#pragma once
// Force-included into every C++ file of the Linux cross build (cmake/toolchains/linux-sniper.cmake): the sniper sysroot ships libstdc++ 10,
// which lacks C++20's std::bit_cast (GCC 11). Remove this shim when the sysroot's libstdc++ has it.
#include <bit>
#include <memory>
#include <type_traits>
#ifndef __cpp_lib_smart_ptr_for_overwrite   // GCC 11 as well
namespace std {
template <class T>
shared_ptr<T> make_shared_for_overwrite() { return shared_ptr<T>(new T); }   // default-initialised: trivial members stay uninitialised
}  // namespace std
#endif
#ifndef __cpp_lib_bit_cast
namespace std {
template <class To, class From>
constexpr enable_if_t<sizeof(To) == sizeof(From) && is_trivially_copyable_v<To> && is_trivially_copyable_v<From>, To> bit_cast(const From& from) noexcept {
    return __builtin_bit_cast(To, from);
}
}  // namespace std
#endif
