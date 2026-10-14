#pragma once
#include "input_prompts.h"
struct ImDrawList;
struct ImVec2;
namespace uc {
void draw_mouse_icon(ImDrawList&,ImVec2 position,ImVec2 size,PhysicalInput,bool both_directions,PromptStyle);
void draw_keycap(ImDrawList&,ImVec2 position,ImVec2 size,PromptStyle);
}
