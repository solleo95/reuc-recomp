#pragma once
#include <cstdint>
namespace uc {
enum class PointerMode:uint8_t { relative,absolute };   // the mouse as motion (default), or the window position of the cursor: lightguns, a Wii Remote bar in mouse mode, tablets
enum class ReloadZone:uint8_t { off,corner,edge };      // a shot fired there reloads instead (lightguns have no off-screen reload of their own)
constexpr uint32_t kPointerSmoothingMax=90;             // percent of the old pointer place that stays at each new absolute position
constexpr uint32_t kReloadZoneSizeMin=1,kReloadZoneSizeMax=20;   // percent of the window's width and height
}
