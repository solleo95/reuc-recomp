#pragma once
// Force-included into every C++ file of the Android build (cmake/toolchains/android-arm64.cmake): the NDK's libc++ (LLVM 18) has no
// floating-point std::from_chars (a double argument falls to the deleted bool overload). These read in the C locale with strtod/strtof, like
// runtime/linux_std_shim.h; to_chars is there and stays libc++'s. Remove when the NDK's libc++ has it.
#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <system_error>
namespace std {
namespace uc_android_shim {
template <class T> from_chars_result parse_text(const char* first, const char* last, T& value, T (*parse)(const char*, char**)) {
    if (first == last || *first == '+' || (unsigned char)*first <= ' ') return {first, errc::invalid_argument};
    char text[128];
    const size_t length = size_t(last - first) < sizeof text - 1 ? size_t(last - first) : sizeof text - 1;
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
}  // namespace uc_android_shim
inline from_chars_result from_chars(const char* first, const char* last, double& value) { return uc_android_shim::parse_text<double>(first, last, value, strtod); }
inline from_chars_result from_chars(const char* first, const char* last, float& value) { return uc_android_shim::parse_text<float>(first, last, value, strtof); }
}  // namespace std
