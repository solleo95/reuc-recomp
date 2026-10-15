#pragma once
#include <chrono>
namespace uc {
// A swapchain rebuild stalls the GPU (device idle, new images). While a window is dragged to a new size the extent changes every frame,
// so rebuilds are spaced by this interval and the old swapchain keeps presenting in between.
constexpr std::chrono::milliseconds kRebuildInterval{60};
inline bool rebuild_due(bool resized, bool have_swapchain, std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point last) {
    return resized && (!have_swapchain || now - last >= kRebuildInterval);
}
}
