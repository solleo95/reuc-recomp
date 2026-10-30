// The highest frame rate limit a platform offers: Linux (the Steam Deck OLED, 90 Hz) caps at 90 and turns "unlimited" into 90; Windows keeps every value.
#include "check.h"
#include "frame_limit_cap.h"
#include "game_pacing.h"
#include <optional>

int main() {
    using uc::clamp_frame_limit;
    CHECK(clamp_frame_limit(std::nullopt) == std::nullopt);   // unset keeps the previous behaviour on every platform
    CHECK(clamp_frame_limit(30) == 30u && clamp_frame_limit(60) == 60u && clamp_frame_limit(90) == 90u);
#ifdef _WIN32
    CHECK(clamp_frame_limit(0) == 0u && clamp_frame_limit(144) == 144u && clamp_frame_limit(1000) == 1000u);
    CHECK(uc::pacing_for_limit(0, 0, false).game_hz == 480u);
    CHECK(uc::pacing_for_limit(240, 0, false).game_hz == 240u);
#else
    CHECK(clamp_frame_limit(0) == 90u);                       // unlimited is 90
    CHECK(clamp_frame_limit(91) == 90u && clamp_frame_limit(144) == 90u && clamp_frame_limit(1000) == 90u);
    CHECK(uc::pacing_for_limit(0, 0, false).game_hz == 90u);  // even a limit that bypassed the clamp steps the game at 90 Hz at most
    CHECK(uc::pacing_for_limit(240, 0, false).game_hz == 90u);
    CHECK(uc::pacing_for_limit(90, 0, false).game_hz == 90u);
#endif
    CHECK(uc::pacing_for_limit(60, 0, false).game_hz == 0u && uc::pacing_for_limit(60, 0, false).unlock_60);   // 31..60: the retrace-locked step
    CHECK(uc::pacing_for_limit(30, 0, false).game_hz == 0u && !uc::pacing_for_limit(30, 0, false).unlock_60);  // 30 or less: the original step
    return 0;
}
