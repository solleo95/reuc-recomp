// The byte-swap intrinsics the runtime uses by their MSVC names must exist on every platform (runtime/compat.h supplies them off Windows).
#include "compat.h"
#include <cstdint>
#include <cstdio>

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* what) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; } };
    check(_byteswap_ushort(0x0102) == 0x0201, "_byteswap_ushort");
    check(_byteswap_ulong(0x01020304u) == 0x04030201u, "_byteswap_ulong");
    check(_byteswap_uint64(0x0102030405060708ull) == 0x0807060504030201ull, "_byteswap_uint64");
    check(_byteswap_ulong(_byteswap_ulong(0xDEADBEEFu)) == 0xDEADBEEFu, "_byteswap_ulong twice is the identity");
    return failures ? 1 : 0;
}
