#pragma once
// MSVC intrinsics by their Windows names, so the runtime and the translated game compile the same on Linux (clang, GNU frontend) and macOS (Apple Silicon).
#ifdef _WIN32
#include <intrin.h>
#else
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#elif defined(__aarch64__)
// The two SSE hints the port uses, as their ARM counterparts: a spin-wait hint and a read prefetch into every cache level.
#define _mm_pause() __builtin_arm_yield()
#define _MM_HINT_T0 3
#define _mm_prefetch(address, hint) __builtin_prefetch((address), 0, (hint))
#endif
#define _ReadWriteBarrier() __asm__ __volatile__("" ::: "memory")
#define _byteswap_ushort(v) __builtin_bswap16(v)
#define _byteswap_ulong(v) __builtin_bswap32(v)
#define _byteswap_uint64(v) __builtin_bswap64(v)
#endif
