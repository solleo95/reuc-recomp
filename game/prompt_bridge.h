#pragma once
#include "gx_vertices.h"
#include "prompt_draw.h"
#include <map>
#include <string_view>
namespace uc {
// Guest baton owns registration and lookup; renderer receives values only.
class PromptRegistry {
public:
    void register_layout(const gx::Memory&,uint32_t layout,std::string_view name);
    void unregister_layout(uint32_t layout);
    void unregister_pane(uint32_t pane);
    std::optional<std::string> layout_name(const gx::Memory&,uint32_t pane) const;
    std::optional<Action> action(const gx::Memory&,uint32_t pane,InputContext) const;
private:
    struct Owner {uint32_t root;std::string name;};
    std::map<uint32_t,Owner> owners_;
};
// Wii button glyphs of the utf8 system fonts; empty for glyphs the port does not produce.
std::vector<Action> glyph_actions(uint16_t code,bool menu);
}
