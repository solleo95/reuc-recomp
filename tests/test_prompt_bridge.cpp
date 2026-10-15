#include "check.h"
#include "ppc_fixture.h"
#include "prompt_bridge.h"
#include <cstring>
#include <limits>
int main() {
    Fix f;auto& c=f.c;
    gx::Memory mem{{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{},{}};
    uc::PromptRegistry registry;
    const uint32_t layout=0x80002000,root=0x80003000,pane=0x80004000;
    ppc::wr32(c,layout+0x10,root);ppc::wr32(c,pane+0x0C,root);
    ppc::wr32(c,pane+0x28,0x80005000);std::memcpy(c.mem+0x4000+0xB8,"tex1",5);
    registry.register_layout(mem,layout,"ACT_A_Press.brlyt");
    CHECK(registry.layout_name(mem,pane)=="ACT_A_Press.brlyt");
    CHECK(registry.action(mem,pane,uc::InputContext::gameplay)==uc::Action::interact);
    CHECK(registry.action(mem,pane,uc::InputContext::menu)==uc::Action::confirm);
    CHECK(!registry.action(mem,pane,uc::InputContext::inactive));
    std::memcpy(c.mem+0x4000+0xB8,"other",6);CHECK(!registry.action(mem,pane,uc::InputContext::gameplay));
    std::memcpy(c.mem+0x4000+0xB8,"tex1",5);
    registry.unregister_layout(layout);CHECK(!registry.action(mem,pane,uc::InputContext::gameplay));
    registry.register_layout(mem,layout,"ACT_B_Press.brlyt");
    CHECK(registry.action(mem,pane,uc::InputContext::gameplay)==uc::Action::fire);
    CHECK(registry.action(mem,pane,uc::InputContext::menu)==uc::Action::back);
    registry.register_layout(mem,layout,"other.brlyt");CHECK(!registry.action(mem,pane,uc::InputContext::menu));
    registry.register_layout(mem,layout,"ACT_AB_Press.brlyt");CHECK(!registry.action(mem,pane,uc::InputContext::qte));
    registry.register_layout(mem,layout,"ACT_R_Hshake.brlyt");
    CHECK(!registry.action(mem,pane,uc::InputContext::gameplay));
    CHECK(registry.action(mem,pane,uc::InputContext::qte)==uc::Action::qte_shake);
    ppc::wr32(c,pane+0x0C,pane);CHECK(!registry.action(mem,pane,uc::InputContext::qte));
    ppc::wr32(c,pane+0x0C,root);ppc::wr32(c,layout+0x10,0x80006000);
    CHECK(!registry.action(mem,pane,uc::InputContext::qte));
    ppc::wr32(c,layout+0x10,root);registry.unregister_pane(root);
    CHECK(!registry.action(mem,pane,uc::InputContext::qte));
    registry.register_layout(mem,layout,"ACT_A_Press.brlyt");
    CHECK(!registry.action(mem,0xCC008000,uc::InputContext::menu));
    ppc::wr32(c,pane+0x28,0xCC008000);CHECK(!registry.action(mem,pane,uc::InputContext::menu));
    ppc::wr32(c,pane+0x28,0x80005000);
    gx::DrawPacket draw;draw.primitive=0x80;draw.vertices.resize(4);draw.indices={0,1,2,0,2,3};
    draw.vertices[0].position={-.5f,-.5f,0};draw.vertices[1].position={.5f,-.5f,0};
    draw.vertices[2].position={.5f,.5f,0};draw.vertices[3].position={-.5f,.5f,0};
    auto description=uc::describe_prompt(uc::default_bindings(),uc::Action::interact);
    auto prompt=uc::prompt_from_quad(draw,description);
    CHECK(prompt && prompt->efb_corners[0]==160 && prompt->efb_corners[1]==120);
    CHECK(prompt->efb_corners[4]==480 && prompt->efb_corners[5]==360);
    draw.vertices[0].color[0][3]=128;prompt=uc::prompt_from_quad(draw,description);
    CHECK(prompt && prompt->alpha[0]==128.f/255);
    draw.vertices[0].color[0]={0,0,0,128};prompt=uc::prompt_from_quad(draw,description);
    CHECK(prompt && prompt->tint[0]==(std::array<float,3>{0,0,0}));
    CHECK(prompt->tint[1]==(std::array<float,3>{1,1,1}) && prompt->alpha[0]==128.f/255);
    draw.transforms.edit().projection[3][3]=0;CHECK(!uc::prompt_from_quad(draw,description));
    draw.transforms=gx::Transforms{};draw.vertices[0].position[0]=std::numeric_limits<float>::quiet_NaN();
    CHECK(!uc::prompt_from_quad(draw,description));
    draw.vertices[0].position[0]=-.5f;draw.indices={0,1,2};CHECK(!uc::prompt_from_quad(draw,description));
    CHECK(ppc::rd32(c,pane+0x0C)==root);
    using uc::Action;
    CHECK(uc::glyph_actions(0x391,false)==std::vector<Action>{Action::interact});
    CHECK(uc::glyph_actions(0x391,true)==std::vector<Action>{Action::confirm});
    CHECK(uc::glyph_actions(0x392,false)==std::vector<Action>{Action::fire});
    CHECK(uc::glyph_actions(0x392,true)==std::vector<Action>{Action::back});
    CHECK(uc::glyph_actions(0x394,true)==(std::vector<Action>{Action::weapon_previous,Action::weapon_next}));
    CHECK(uc::glyph_actions(0x393,false)==std::vector<Action>{Action::weapon_next});
    CHECK(uc::glyph_actions(0x39E,true)==std::vector<Action>{Action::back});
    CHECK(uc::glyph_actions(0x39E,false)==std::vector<Action>{Action::pause});
    for(uint16_t code:{0x396,0x398,0x41,0x3A9,0x2605})CHECK(uc::glyph_actions(code,false).empty());
    return 0;
}
