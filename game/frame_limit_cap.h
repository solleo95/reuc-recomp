#pragma once
#include <cstdint>
#include <optional>

namespace uc {
// The highest frame rate limit the platform offers. Linux (the Steam Deck OLED: a 90 Hz display) stops at 90, and "unlimited" there means 90; Windows has no
// cap (0), so every limit and "unlimited" stay as they were.
#ifdef _WIN32
inline constexpr uint32_t kMaxFrameLimit = 0;
#else
inline constexpr uint32_t kMaxFrameLimit = 90;
#endif
// A limit as the platform offers it: unset stays unset; with a cap, 0 (unlimited) and everything above the cap become the cap.
inline std::optional<uint32_t> clamp_frame_limit(std::optional<uint32_t> limit) {
    if (!kMaxFrameLimit || !limit) return limit;
    return (*limit == 0 || *limit > kMaxFrameLimit) ? kMaxFrameLimit : *limit;
}
}  // namespace uc
