#pragma once
#include <cstdint>
// The only place that holds guest structure offsets. Every value is proven by a disassembly line in docs/spikes/R0-layouts.md.
namespace uc::layout {
// OSThread (RVL SDK)
constexpr uint32_t kThreadState = 0x2C8;        // u16: 1 ready, 2 running, 4 waiting, 8 moribund
constexpr uint32_t kThreadAttr = 0x2CA;         // u16
constexpr uint32_t kThreadSuspend = 0x2CC;      // s32
constexpr uint32_t kThreadPriority = 0x2D0;     // s32
constexpr uint32_t kThreadBasePriority = 0x2D4; // s32
constexpr uint32_t kThreadStackBase = 0x304;    // u32: top of the stack
constexpr uint32_t kThreadStackEnd = 0x308;     // u32: lowest address, holds the 0xDEADBABE canary
constexpr uint32_t kCurrentThreadPtr = 0x800000E4;   // low memory: OSGetCurrentThread() reads this word
// DVDFileInfo / command block
constexpr uint32_t kCbState = 0x0C;
constexpr uint32_t kFiStart = 0x30;
constexpr uint32_t kFiLength = 0x34;
constexpr uint32_t kFiCallback = 0x38;
}  // namespace uc::layout
