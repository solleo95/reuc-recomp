#include "pad_glyphs.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
namespace uc {
namespace {
constexpr ImU32 rgb(int r,int g,int b) { return IM_COL32(r,g,b,255); }
bool is_button(PhysicalInput input) { return input.kind==PhysicalKind::pad_button; }
bool is_axis(PhysicalInput input) { return input.kind==PhysicalKind::pad_axis; }
bool is_face(PhysicalInput input) { return is_button(input) && input.code<4; }
bool is_shoulder(PhysicalInput input) { return is_button(input) && (input.code==9 || input.code==10); }
bool is_trigger(PhysicalInput input) { return is_axis(input) && input.code>=8 && input.code<12; }
bool is_stick_click(PhysicalInput input) { return is_button(input) && (input.code==7 || input.code==8); }
bool is_stick_direction(PhysicalInput input) { return is_axis(input) && input.code<8; }
bool is_dpad(PhysicalInput input) { return is_button(input) && input.code>=11 && input.code<=14; }
void text_centered(ImDrawList& list,ImVec2 center,float font_size,ImU32 color,const std::string& text) {
    auto* font=ImGui::GetFont();
    const ImVec2 extent=font->CalcTextSizeA(font_size,FLT_MAX,0,text.c_str());
    list.AddText(font,font_size,{center.x-extent.x/2,center.y-extent.y/2},color,text.c_str());
}
void face_button(ImDrawList& list,ImVec2 center,float r,unsigned code,PadFamily family) {
    switch(family) {
    case PadFamily::xbox: {
        static constexpr ImU32 letters[4]={rgb(107,190,65),rgb(232,64,56),rgb(60,140,230),rgb(246,200,40)};   // A green, B red, X blue, Y yellow
        list.AddCircleFilled(center,r,rgb(28,30,33),24); list.AddCircle(center,r,rgb(120,124,128),24,r*.08f);
        text_centered(list,center,r*1.35f,letters[code],pad_glyph_label({PhysicalKind::pad_button,uint16_t(code)},family));
        break;
    }
    case PadFamily::playstation: {
        list.AddCircleFilled(center,r,rgb(30,33,46),24); list.AddCircle(center,r,rgb(90,96,120),24,r*.08f);
        const float s=r*.42f,t=r*.16f;
        if(code==0) {   // cross, blue
            list.AddLine({center.x-s,center.y-s},{center.x+s,center.y+s},rgb(124,178,232),t); list.AddLine({center.x-s,center.y+s},{center.x+s,center.y-s},rgb(124,178,232),t);
        } else if(code==1) list.AddCircle(center,s,rgb(255,102,102),24,t);   // circle, red
        else if(code==2) list.AddRect({center.x-s*.9f,center.y-s*.9f},{center.x+s*.9f,center.y+s*.9f},rgb(232,140,200),0,0,t);   // square, pink
        else list.AddTriangle({center.x,center.y-s*1.1f},{center.x-s*1.05f,center.y+s*.85f},{center.x+s*1.05f,center.y+s*.85f},rgb(64,226,160),t);   // triangle, green
        break;
    }
    case PadFamily::nintendo:
        list.AddCircleFilled(center,r,rgb(236,236,236),24); list.AddCircle(center,r,rgb(60,60,60),24,r*.08f);
        text_centered(list,center,r*1.3f,rgb(30,30,30),pad_glyph_label({PhysicalKind::pad_button,uint16_t(code)},family));
        break;
    case PadFamily::generic: {   // the four positions as a diamond, the pressed one lit: south 0, east 1, west 2, north 3
        list.AddCircleFilled(center,r,rgb(58,60,64),24); list.AddCircle(center,r,rgb(130,132,136),24,r*.08f);
        const ImVec2 spots[4]={{0,.5f},{.5f,0},{-.5f,0},{0,-.5f}};
        for(unsigned i=0;i<4;++i) list.AddCircleFilled({center.x+spots[i].x*r,center.y+spots[i].y*r},r*.17f,i==code ? IM_COL32_WHITE : rgb(110,112,116),12);
        break;
    }
    }
}
// A pill with the controller's own name for the input on it.
void label_pill(ImDrawList& list,ImVec2 p,ImVec2 size,PhysicalInput input,PadFamily family,ImDrawFlags corners,ImU32 fill_xbox,ImU32 fill_sony) {
    const float rounding=size.y*.35f;
    const ImU32 fill=family==PadFamily::nintendo ? rgb(210,210,214) : family==PadFamily::playstation ? fill_sony : fill_xbox;
    const ImU32 ring=family==PadFamily::nintendo ? rgb(70,70,74) : rgb(150,154,160);
    list.AddRectFilled(p,{p.x+size.x,p.y+size.y},fill,rounding,corners); list.AddRect(p,{p.x+size.x,p.y+size.y},ring,rounding,corners,size.y*.05f);
    text_centered(list,{p.x+size.x/2,p.y+size.y/2},size.y*.52f,family==PadFamily::nintendo ? rgb(30,30,30) : IM_COL32_WHITE,pad_glyph_label(input,family));
}
void stick_glyph(ImDrawList& list,ImVec2 center,float r,PhysicalInput input,PadFamily family) {
    list.AddCircleFilled(center,r,family==PadFamily::nintendo ? rgb(200,200,204) : rgb(40,43,48),24);
    list.AddCircle(center,r,rgb(150,154,160),24,r*.08f);
    if(is_stick_click(input)) {
        list.AddCircleFilled(center,r*.62f,family==PadFamily::nintendo ? rgb(150,150,154) : rgb(70,74,80),20);
        text_centered(list,center,r*.8f,IM_COL32_WHITE,pad_glyph_label(input,family));
        return;
    }
    const unsigned axis=input.code/2; const bool positive=input.code&1;   // x: left/right, y: up/down (SDL y points down)
    const float dx=(axis&1)==0 ? (positive ? 1.f : -1.f) : 0,dy=(axis&1)==1 ? (positive ? 1.f : -1.f) : 0;
    const ImVec2 tip{center.x+dx*r*.78f,center.y+dy*r*.78f},base{center.x-dx*r*.1f,center.y-dy*r*.1f};
    const ImVec2 side{-dy*r*.4f,dx*r*.4f};
    list.AddTriangleFilled(tip,{base.x+side.x,base.y+side.y},{base.x-side.x,base.y-side.y},IM_COL32_WHITE);
}
void dpad(ImDrawList& list,ImVec2 p,ImVec2 size,unsigned code,PadFamily family) {
    const float arm=std::min(size.x,size.y)*.34f,half=arm/2;
    const ImVec2 c{p.x+size.x/2,p.y+size.y/2};
    const float reach=std::min(size.x,size.y)*.48f;
    const ImU32 idle=family==PadFamily::nintendo ? rgb(190,190,194) : rgb(60,64,70);
    const ImU32 lit=family==PadFamily::nintendo ? rgb(40,40,44) : rgb(235,235,238);
    struct Arm {ImVec2 a,b;} arms[4]={{{c.x-half,c.y-reach},{c.x+half,c.y-half}},{{c.x-half,c.y+half},{c.x+half,c.y+reach}},{{c.x-reach,c.y-half},{c.x-half,c.y+half}},{{c.x+half,c.y-half},{c.x+reach,c.y+half}}};   // up, down, left, right
    list.AddRectFilled({c.x-half,c.y-half},{c.x+half,c.y+half},idle);
    for(unsigned i=0;i<4;++i) list.AddRectFilled(arms[i].a,arms[i].b,i==code-11 ? lit : idle,arm*.2f);
}
void small_button(ImDrawList& list,ImVec2 p,ImVec2 size,PhysicalInput input,PadFamily family) {   // start / back: the controller's own small symbol
    const float h=size.y*.62f; const ImVec2 q{p.x+size.x*.1f,p.y+(size.y-h)/2},e{p.x+size.x*.9f,q.y+h};
    const bool nintendo=family==PadFamily::nintendo;
    list.AddRectFilled(q,e,nintendo ? rgb(210,210,214) : rgb(46,49,55),h*.5f); list.AddRect(q,e,rgb(150,154,160),h*.5f,0,h*.06f);
    const ImVec2 c{(q.x+e.x)/2,(q.y+e.y)/2}; const ImU32 ink=nintendo ? rgb(30,30,30) : IM_COL32_WHITE;
    if(nintendo) {text_centered(list,c,h*.95f,ink,pad_glyph_label(input,family));return;}
    if(input.code==6) for(int i=-1;i<=1;++i) list.AddLine({c.x-h*.28f,c.y+i*h*.2f},{c.x+h*.28f,c.y+i*h*.2f},ink,h*.07f);   // menu / options: three lines
    else {   // view / create / back: two overlapping rectangles
        list.AddRect({c.x-h*.3f,c.y-h*.22f},{c.x+h*.12f,c.y+h*.16f},ink,h*.05f,0,h*.07f);
        list.AddRect({c.x-h*.12f,c.y-h*.12f},{c.x+h*.3f,c.y+h*.26f},ink,h*.05f,0,h*.07f);
    }
}
}
float pad_glyph_aspect(PhysicalInput input) {
    if(is_shoulder(input) || is_trigger(input)) return 1.8f;
    if(is_button(input) && (input.code==4 || input.code==6)) return 1.5f;
    if(is_button(input) && input.code>=15) return 2.f;
    if(is_axis(input) && input.code>=12) return 1.5f;
    return 1.f;
}
float gyro_icon_aspect() { return 1.5f; }
void draw_pad_glyph(ImDrawList& list,ImVec2 position,ImVec2 size,PhysicalInput input,PadFamily family) {
    if(!is_button(input) && !is_axis(input)) return;
    const ImVec2 center{position.x+size.x/2,position.y+size.y/2};
    const float r=std::min(size.x,size.y)/2*.96f;
    if(is_face(input)) face_button(list,center,r,input.code,family);
    else if(is_shoulder(input)) label_pill(list,position,size,input,family,ImDrawFlags_RoundCornersAll,rgb(74,78,84),rgb(40,44,56));
    else if(is_trigger(input)) label_pill(list,position,size,input,family,ImDrawFlags_RoundCornersTop,rgb(60,64,70),rgb(34,38,50));
    else if(is_stick_click(input) || is_stick_direction(input)) stick_glyph(list,center,r,input,family);
    else if(is_dpad(input)) dpad(list,position,size,input.code,family);
    else if(is_button(input) && (input.code==4 || input.code==6)) small_button(list,position,size,input,family);
    else label_pill(list,position,size,input,family,ImDrawFlags_RoundCornersAll,rgb(60,64,70),rgb(40,44,56));
}
void draw_gyro_icon(ImDrawList& list,ImVec2 position,ImVec2 size,PadFamily family) {
    const ImVec2 c{position.x+size.x/2,position.y+size.y/2};
    const float h=size.y,w=h*.95f;   // the controller
    const ImU32 body=family==PadFamily::nintendo ? rgb(200,200,204) : family==PadFamily::playstation ? rgb(46,50,66) : rgb(44,47,52);
    const ImVec2 a{c.x-w/2,c.y-h*.2f},b{c.x+w/2,c.y+h*.16f};
    list.AddRectFilled(a,b,body,h*.16f); list.AddCircleFilled({a.x+h*.14f,b.y+h*.08f},h*.2f,body,16); list.AddCircleFilled({b.x-h*.14f,b.y+h*.08f},h*.2f,body,16);
    list.AddRect(a,b,rgb(170,174,180),h*.16f,0,h*.04f);
    list.AddCircleFilled({c.x-w*.28f,c.y-h*.02f},h*.05f,rgb(170,174,180),8); list.AddCircleFilled({c.x+w*.28f,c.y-h*.02f},h*.05f,rgb(170,174,180),8);
    const ImU32 arrow=rgb(90,190,255); const float rr=h*.5f,pi=3.14159265f;   // turning arrows on both sides
    for(int side:{-1,1}) {
        const float cx=c.x+side*(w*.5f+h*.12f);
        list.PathArcTo({cx-side*h*.25f,c.y},rr*.7f,side<0 ? pi*.62f : -pi*.38f,side<0 ? pi*1.38f : pi*.38f);
        list.PathStroke(arrow,0,h*.07f);
        const ImVec2 tip{cx+side*h*.1f,c.y-rr*.62f};
        list.AddTriangleFilled(tip,{tip.x-side*h*.18f,tip.y-h*.02f},{tip.x-side*h*.02f,tip.y+h*.16f},arrow);
    }
}
}
