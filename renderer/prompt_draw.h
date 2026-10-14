#pragma once
#include "input_prompts.h"
#include <array>
namespace gx {
struct PromptDraw {
    uc::PromptDescription description;
    std::array<float,8> efb_corners{};
    std::array<float,4> alpha{1,1,1,1};
    std::array<float,4> clip{0,0,640,480};
    bool compact=false; // inline text glyph: primary binding, icon-only mouse
    std::array<std::array<float,3>,4> tint{{{1,1,1},{1,1,1},{1,1,1},{1,1,1}}};
};
struct DrawPacket;
}
namespace uc {
// Output-space prompt over one validated SDK quad; empty for anything else.
std::optional<gx::PromptDraw> prompt_from_quad(const gx::DrawPacket&,const PromptDescription&);
}
