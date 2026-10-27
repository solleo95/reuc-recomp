#pragma once
#include "prompt_draw.h"
#include "render_scale.h"

namespace uc {
// Call with the overlay's ImGui context current; retain font_bytes until it is destroyed.
void load_prompt_font(std::vector<char>& font_bytes);
void draw_prompt_picture(const gx::PromptDraw&,RenderExtent,const std::array<uint32_t,4>& source,PromptStyle);
}
