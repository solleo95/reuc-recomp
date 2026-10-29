#include "check.h"
#include "pad_glyphs.h"
#include "imgui.h"
#include <functional>
using namespace uc;
// Draws one glyph on a background list and reports the vertices it made; the context is built the way the settings menu test builds one (no GPU).
struct Canvas {
    Canvas() {
        ImGui::CreateContext(); auto& io=ImGui::GetIO(); io.DisplaySize={640,480}; io.DeltaTime=1.f/60; io.IniFilename=nullptr;
        io.BackendFlags|=ImGuiBackendFlags_RendererHasTextures;
        ImGui::NewFrame();
    }
    ~Canvas() { ImGui::EndFrame(); ImGui::DestroyContext(); }
    ImDrawList& list() { return *ImGui::GetBackgroundDrawList(); }
};
static std::vector<ImU32> colors(ImDrawList& list,const std::function<void()>& draw) {
    const int first=list.VtxBuffer.Size; draw();
    std::vector<ImU32> result; for(int i=first;i<list.VtxBuffer.Size;++i) result.push_back(list.VtxBuffer[i].col);
    return result;
}
static bool any(const std::vector<ImU32>& list,const std::function<bool(int,int,int)>& match) {
    for(auto c:list) if((c>>24)>0 && match(int(c&255),int((c>>8)&255),int((c>>16)&255))) return true;
    return false;
}
static PhysicalInput button(uint16_t code) { return {PhysicalKind::pad_button,code}; }
static PhysicalInput axis(uint16_t code) { return {PhysicalKind::pad_axis,code}; }
int main() {
    Canvas canvas; auto& list=canvas.list(); const ImVec2 at{10,10},size{40,40};
    // Every input of the defaults, in every family, draws something and has a sensible shape.
    std::vector<PhysicalInput> inputs;
    for(uint16_t code:{0,1,2,3,4,6,7,8,9,10,11,12,13,14}) inputs.push_back(button(code));
    for(uint16_t code:{0,1,2,3,4,5,6,7,9,11}) inputs.push_back(axis(code));
    for(auto family:{PadFamily::xbox,PadFamily::playstation,PadFamily::nintendo,PadFamily::generic}) {
        for(auto input:inputs) {
            CHECK(pad_glyph_aspect(input)>=.8f && pad_glyph_aspect(input)<=2.5f);
            CHECK(!colors(list,[&]{draw_pad_glyph(list,at,{size.y*pad_glyph_aspect(input),size.y},input,family);}).empty());
        }
        CHECK(!colors(list,[&]{draw_gyro_icon(list,at,{size.y*gyro_icon_aspect(),size.y},family);}).empty());
    }
    // The look of each family, by its colours: Xbox buttons are dark with a green A, PlayStation has a blue cross, Nintendo's buttons are light.
    auto xbox_a=colors(list,[&]{draw_pad_glyph(list,at,size,button(0),PadFamily::xbox);});
    CHECK(any(xbox_a,[](int r,int g,int b){return g>r+40 && g>b+40;}));                             // the green A
    auto xbox_b=colors(list,[&]{draw_pad_glyph(list,at,size,button(1),PadFamily::xbox);});
    CHECK(any(xbox_b,[](int r,int g,int b){return r>g+60 && r>b+60;}));                             // the red B
    auto xbox_y=colors(list,[&]{draw_pad_glyph(list,at,size,button(3),PadFamily::xbox);});
    CHECK(any(xbox_y,[](int r,int g,int b){return r>180 && g>150 && b<90;}));                       // the yellow Y
    auto cross=colors(list,[&]{draw_pad_glyph(list,at,size,button(0),PadFamily::playstation);});
    CHECK(any(cross,[](int r,int g,int b){return b>r+60;}) && !any(cross,[](int r,int g,int b){return g>r+40 && g>b+40;}));   // the blue cross, no Xbox green
    auto circle=colors(list,[&]{draw_pad_glyph(list,at,size,button(1),PadFamily::playstation);});
    CHECK(any(circle,[](int r,int g,int b){return r>g+60 && r>b+40;}));                             // the red circle
    auto nintendo=colors(list,[&]{draw_pad_glyph(list,at,size,button(0),PadFamily::nintendo);});
    CHECK(any(nintendo,[](int r,int g,int b){return r>200 && g>200 && b>200;}));                     // light button faces
    // A held shoulder button or trigger is wider than tall; a face button is round.
    CHECK(pad_glyph_aspect(button(9))>1.3f && pad_glyph_aspect(axis(11))>1.3f && pad_glyph_aspect(button(0))==1.f);
    // The Wii Remote's buttons: each draws, in the remote's white (the A, the D-pad, + - 1 2 Home) or the nunchuk's dark grey (Z, C and the B trigger); round ones are round.
    for(uint16_t code=0;code<kWiiButtonCount;++code) {
        const PhysicalInput wii{PhysicalKind::wii_button,code};
        CHECK(pad_glyph_aspect(wii)>=.8f && pad_glyph_aspect(wii)<=2.5f);
        CHECK(!colors(list,[&]{draw_pad_glyph(list,at,{size.y*pad_glyph_aspect(wii),size.y},wii,PadFamily::xbox);}).empty());
    }
    const PhysicalInput wii_a{PhysicalKind::wii_button,8},wii_z{PhysicalKind::wii_button,11},wii_b{PhysicalKind::wii_button,7};
    CHECK(any(colors(list,[&]{draw_pad_glyph(list,at,size,wii_a,PadFamily::xbox);}),[](int r,int g,int b){return r>200 && g>200 && b>200;}));   // white plastic
    CHECK(pad_glyph_aspect(wii_z)>1.2f && pad_glyph_aspect(wii_b)>1.2f && pad_glyph_aspect(wii_a)==1.f);
    CHECK(colors(list,[&]{draw_pad_glyph(list,at,size,wii_a,PadFamily::nintendo);}).size()==colors(list,[&]{draw_pad_glyph(list,at,size,wii_a,PadFamily::playstation);}).size());   // the family of a gamepad does not change the remote
    CHECK(!colors(list,[&]{draw_wii_motion_icon(list,at,{size.y*gyro_icon_aspect(),size.y});}).empty());
    // Nothing the prompt does not know is drawn: a key is not a gamepad input.
    CHECK(colors(list,[&]{draw_pad_glyph(list,at,size,{PhysicalKind::key,4},PadFamily::xbox);}).empty());
}
