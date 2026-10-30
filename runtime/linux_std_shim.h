#pragma once
// Force-included into every C++ file of the Linux cross build (cmake/toolchains/linux-sniper.cmake): the sniper sysroot ships libstdc++ 10,
// which lacks C++20's std::bit_cast (GCC 11). Remove this shim when the sysroot's libstdc++ has it.
#include <bit>
#include <memory>
#include <type_traits>
#include <charconv>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <system_error>
#ifndef __cpp_lib_to_chars   // floating-point to_chars/from_chars arrived with GCC 11; these use printf/strtod in the C locale
namespace std {
namespace uc_shim {
template <class T> to_chars_result print_shortest(char* first, char* last, T value, T (*parse)(const char*, char**)) {
    char text[48];
    int length = 0;
    for (int precision = 1; precision <= 17; ++precision) {   // the shortest text that reads back as the same value
        length = snprintf(text, sizeof text, "%.*g", precision, double(value));
        if (parse(text, nullptr) == value) break;
    }
    if (length > last - first) return {last, errc::value_too_large};
    memcpy(first, text, size_t(length));
    return {first + length, errc{}};
}
template <class T> from_chars_result parse_text(const char* first, const char* last, T& value, T (*parse)(const char*, char**)) {
    if (first == last || *first == '+' || (unsigned char)*first <= ' ') return {first, errc::invalid_argument};
    char text[128];
    const size_t length = min(size_t(last - first), sizeof text - 1);
    memcpy(text, first, length);
    text[length] = 0;
    char* end = nullptr;
    errno = 0;
    const T parsed = parse(text, &end);
    if (end == text) return {first, errc::invalid_argument};
    const bool out_of_range = errno == ERANGE;
    if (!out_of_range) value = parsed;
    return {first + (end - text), out_of_range ? errc::result_out_of_range : errc{}};
}
}  // namespace uc_shim
inline to_chars_result to_chars(char* first, char* last, double value) { return uc_shim::print_shortest<double>(first, last, value, strtod); }
inline to_chars_result to_chars(char* first, char* last, float value) { return uc_shim::print_shortest<float>(first, last, value, strtof); }
inline from_chars_result from_chars(const char* first, const char* last, double& value) { return uc_shim::parse_text<double>(first, last, value, strtod); }
inline from_chars_result from_chars(const char* first, const char* last, float& value) { return uc_shim::parse_text<float>(first, last, value, strtof); }
}  // namespace std
#endif
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
