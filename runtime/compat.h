#pragma once
// MSVC intrinsics by their Windows names, so the runtime and the translated game compile the same on Linux (clang, GNU frontend).
#ifdef _WIN32
#include <intrin.h>
#else
#include <x86intrin.h>
#define _ReadWriteBarrier() __asm__ __volatile__("" ::: "memory")
#define _byteswap_ushort(v) __builtin_bswap16(v)
#define _byteswap_ulong(v) __builtin_bswap32(v)
#define _byteswap_uint64(v) __builtin_bswap64(v)
#endif
