#pragma once
#include <atomic>
#include <cstdint>
namespace uc {
// The field of view (option fov): the percentage the tangent of the game's own half field of view is scaled by in every C_MTXPerspective (game/intro.cpp), 100 = the game's own.
// Like the native ultrawide it changes what the game computes (its projection, the pointer's range, hit tests, culling): in a network game both PCs must use the same value
// (the host's, see netplay_game.cpp), in a deterministic run or a demo it stays 100.
constexpr uint32_t kFovMin=50,kFovMax=300,kFovDefault=100;
inline std::atomic<uint32_t>& fov_state() { static std::atomic<uint32_t> percent{kFovDefault}; return percent; }
inline void set_view_fov(uint32_t percent) { fov_state().store(percent<kFovMin ? kFovMin : percent>kFovMax ? kFovMax : percent); }
inline uint32_t view_fov() { return fov_state().load(); }
}
