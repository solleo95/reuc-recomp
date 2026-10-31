#pragma once
#include <cstdint>
#include <optional>

namespace uc {
// The highest frame rate limit ordinary launches offer. Linux (the Steam Deck OLED: a 90 Hz display) stops at 90, and "unlimited" there means 90; Windows has no
// cap (0), so every limit and "unlimited" stay as they were.
#ifdef _WIN32
inline constexpr uint32_t kMaxFrameLimit = 0;
#else
inline constexpr uint32_t kMaxFrameLimit = 90;
#endif
// Test-server CLI runs may explicitly request unlimited; config and ordinary launches keep the platform cap.
inline std::optional<uint32_t> clamp_frame_limit(std::optional<uint32_t> limit,bool test_unlimited=false) {
    if (!kMaxFrameLimit || !limit || (test_unlimited && *limit==0)) return limit;
    return (*limit == 0 || *limit > kMaxFrameLimit) ? kMaxFrameLimit : *limit;
}
}  // namespace uc
