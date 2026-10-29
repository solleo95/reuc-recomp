#pragma once
#include "input_prompts.h"
struct ImDrawList;
struct ImVec2;
namespace uc {
// Gamepad inputs drawn the way the controller's own buttons look (Xbox: dark buttons with the green A, red B, blue X, yellow Y; PlayStation: dark buttons with the
// blue cross, red circle, pink square and green triangle; Nintendo: light buttons with black letters; the generic family: dark buttons by position), whatever the
// prompt style: that is the game's look for the keyboard and mouse only. The Wii Remote's buttons (and the nunchuk's) are drawn as they look on it, whatever the family.
// A non-gamepad input draws nothing.
void draw_pad_glyph(ImDrawList&,ImVec2 position,ImVec2 size,PhysicalInput,PadFamily);
// A controller with turning arrows: the gyro gestures (shake, swing) of the gamepad.
void draw_gyro_icon(ImDrawList&,ImVec2 position,ImVec2 size,PadFamily);
// A Wii Remote with turning arrows: its swing and shake gestures (same size as the gamepad's: gyro_icon_aspect()).
void draw_wii_motion_icon(ImDrawList&,ImVec2 position,ImVec2 size);
float pad_glyph_aspect(PhysicalInput);   // width / height: 1 for round buttons, wider for shoulder buttons and triggers
float gyro_icon_aspect();
}
