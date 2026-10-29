#include "prompt_picture.h"
#include "prompt_icons.h"
#include "pad_glyphs.h"
#include "wide_view.h"
#include <intrin.h>
#include <SDL3/SDL.h>
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
namespace uc {
namespace {
struct Token {std::string label;std::optional<PhysicalInput> input;bool cap=false;bool both_directions=false;bool gyro=false;};   // gyro: the gamepad's motion icon (the gesture of a shake or swing)
bool pad_token(const Token& token) {return token.input && (token.input->kind==PhysicalKind::pad_button || token.input->kind==PhysicalKind::pad_axis || token.input->kind==PhysicalKind::wii_button);}
std::string physical_name(PhysicalInput input) {
    if(input.kind==PhysicalKind::mouse) {
        if(input.code==1)return "ЛКМ";
        if(input.code==3)return "ПКМ";
        if(input.code==2)return "Колесо";
        return "Мышь "+std::to_string(input.code);
    }
    if(input.kind==PhysicalKind::wheel)return input.code==1 ? "Колесо вверх" : "Колесо вниз";
    const char* name=SDL_GetScancodeName(SDL_Scancode(input.code));
    return name && *name ? name : "Клавиша "+std::to_string(input.code);
}
// Compact: inline text glyph cell; mouse/wheel are icon-only and alternatives share one row.
std::vector<std::vector<Token>> tokens(const PromptDescription& description,bool compact) {
    std::vector<std::vector<Token>> rows;
    if(description.alternatives.empty())return {{{compact ? "—" : "Не назначено",{},false}}};
    // Only the first assigned input of an action is shown: no "or" rows, no alternative bindings. Mouse and wheel are an icon without a caption.
    for(const auto& alternative:description.alternatives) {
        if(compact && !rows.empty())rows.back().push_back({"/",{},false});   // only the weapon pair (wheel up / wheel down) reaches this
        else if(!rows.empty())break;
        auto& row=compact && !rows.empty() ? rows.back() : rows.emplace_back();
        for(const auto& choice:alternative.choices) {
            if(!row.empty() && row.back().label!="/")row.push_back({"+",{},false});
            for(size_t i=0;i<choice.bindings.size() && (i==0 || compact);++i) {
                if(i)row.push_back({"/",{},false});
                const auto& binding=choice.bindings[i];
                for(const auto& modifier:binding.modifiers) {
                    row.push_back({modifier.kind!=PhysicalKind::key ? "" : physical_name(modifier),modifier,true});row.push_back({"+",{},false});
                }
                const bool icon=binding.input.kind!=PhysicalKind::key;
                row.push_back({icon ? "" : physical_name(binding.input),binding.input,true});
            }
        }
        if(alternative.gesture) {   // a mouse picture (a gamepad with turning arrows for a gamepad) stands in for the Wii Remote of the shake/swing prompts and moves with its pane
            if(!row.empty() && row.back().label!="/")row.push_back({"+",{},false});
            if(description.device.gamepad || description.device.wiimote)row.push_back({"",{},true,false,true});
            else row.push_back({"",PhysicalInput{PhysicalKind::mouse,0},true});
        }
    }
    // "Control Pad up/down" bound to wheel up and wheel down reads as one two-way wheel icon.
    if(compact && rows.size()==1 && rows[0].size()==3 && rows[0][1].label=="/") {
        const auto& a=rows[0][0];const auto& b=rows[0][2];
        if(a.input && b.input && a.input->kind==PhysicalKind::wheel && b.input->kind==PhysicalKind::wheel && a.input->code!=b.input->code) {
            auto merged=a;merged.both_directions=true;rows[0]={merged};
        }
    }
    return rows;
}
float length(ImVec2 a,ImVec2 b) {return std::hypot(a.x-b.x,a.y-b.y);}
void picture(ImDrawList& list,const gx::PromptDraw& prompt,RenderExtent extent,const std::array<uint32_t,4>& source,PromptStyle style) {
    const auto place_x=[&](float x){return (x-source[0])*extent.width/source[2];};
    const auto place_y=[&](float y){return (y-source[1])*extent.height/source[3];};
    std::array<ImVec2,4> corners;
    for(size_t i=0;i<4;++i)corners[i]={place_x(prompt.efb_corners[i*2]),place_y(prompt.efb_corners[i*2+1])};
    // Native ultrawide: the prompts' corners were narrowed with the text line they stand in when the packet was sealed (renderer/ultrawide.h).
    const float width=length(corners[0],corners[1]),height=length(corners[0],corners[3]);
    if(width<1 || height<1)return;
    const auto rows=tokens(prompt.description,prompt.compact);
    const float pad=prompt.compact ? 1 : 4;
    auto* font=ImGui::GetFont();
    const float density=float(extent.height)/720;
    float font_size=height*.6f;   // icons fill the pane of the original picture; the fit loop below shrinks wide combinations
    const PadFamily family=prompt.description.device.family;
    auto has_icon=[](const Token& token){return token.cap && !token.gyro && !pad_token(token) && (!token.input || token.input->kind!=PhysicalKind::key);};
    auto mouse=[](const Token& token){return token.input && token.input->kind!=PhysicalKind::key && !pad_token(token);};
    auto token_width=[&](const Token& token) {
        if(pad_token(token))return pad_glyph_aspect(*token.input)*font_size*1.2f;   // gamepad inputs are drawn in the look of the controller, not as keys
        if(token.gyro)return gyro_icon_aspect()*font_size*1.2f;
        const float label=font->CalcTextSizeA(font_size,FLT_MAX,0,token.label.c_str()).x;
        if(mouse(token))return label+font_size*.85f;
        return label+(token.cap ? font_size*.8f : 0)+(has_icon(token) ? font_size : 0);
    };
    float used_width=0;
    auto layout=[&](bool draw) {
        used_width=0;
        float x=pad,y=pad;
        for(const auto& row:rows) {
            for(const auto& token:row) {
                const float w=token_width(token),h=font_size*1.55f;
                if(!prompt.compact && x>pad && x+w>width-pad) {x=pad;y+=h;}
                if(draw && pad_token(token)) draw_pad_glyph(list,{x,y+font_size*.2f},{w,font_size*1.2f},*token.input,family);
                else if(draw && token.gyro) {
                    if(prompt.description.device.wiimote) draw_wii_motion_icon(list,{x,y+font_size*.2f},{w,font_size*1.2f});
                    else draw_gyro_icon(list,{x,y+font_size*.2f},{w,font_size*1.2f},family);
                }
                else if(draw) {
                    if(token.cap && !mouse(token))draw_keycap(list,{x,y},{w,h},style);
                    float text_x=x+(token.cap && !mouse(token) ? font_size*.4f : 0);
                    if(mouse(token)) {
                        const bool wheel=token.input->kind==PhysicalKind::wheel;
                        draw_mouse_icon(list,{x,y+font_size*(wheel ? .2f : .1f)},{font_size*.85f,font_size*(wheel ? 1.15f : 1.35f)},*token.input,token.both_directions,style);
                        text_x+=font_size*1.1f;
                    } else if(has_icon(token)) {
                        const ImVec2 points[]={{text_x,y+font_size},{text_x+font_size*.25f,y+font_size*.35f},{text_x+font_size*.5f,y+font_size},{text_x+font_size*.75f,y+font_size*.35f}};
                        list.AddPolyline(points,4,IM_COL32(90,190,255,255),2*density,0);text_x+=font_size;
                    }
                    list.AddText(font,font_size,{text_x,y+font_size*.25f},IM_COL32_WHITE,token.label.c_str());
                }
                x+=w+font_size*.25f;
                used_width=std::max(used_width,x);
            }
            x=pad;y+=font_size*1.55f;
        }
        return y;
    };
    for(int attempt=0;attempt<8;++attempt) {
        float widest=0;for(const auto& row:rows)for(const auto& token:row)widest=std::max(widest,token_width(token));
        const float ratio=std::max(layout(false)/std::max(1.f,height-pad),widest/std::max(1.f,width-2*pad));
        if(ratio<=1)break;
        font_size/=ratio*1.02f;
    }
    const float left=std::clamp(place_x(prompt.clip[0]),0.f,float(extent.width));
    const float top=std::clamp(place_y(prompt.clip[1]),0.f,float(extent.height));
    const float right=std::clamp(place_x(prompt.clip[2]),left,float(extent.width));
    const float bottom=std::clamp(place_y(prompt.clip[3]),top,float(extent.height));
    // ImGui clips text on the CPU before its vertices receive the pane transform.
    list.PushClipRect({-1,-1},{width+1,height+1},false);
    const int first_command=list.CmdBuffer.Size-1;
    const int first=list.VtxBuffer.Size;
    const float content_height=layout(false);
    float offset_x=(width-used_width)/2;
    const float offset_y=(height-content_height)/2;
    if(rows.size()==1 && rows[0].size()==1 && mouse(rows[0][0]) &&
       corners[0].y==corners[1].y && corners[0].x==corners[3].x && corners[1].x>corners[0].x && corners[3].y>corners[0].y) {
        // A pixel-aligned mouse center keeps mirrored button coverage equal at HUD sizes.
        const float center=corners[0].x+pad+font_size*.425f+offset_x;
        offset_x+=std::round(center)-center;
    }
    layout(true);
    for(int i=first;i<list.VtxBuffer.Size;++i) {
        auto& v=list.VtxBuffer[i];const float u=(v.pos.x+offset_x)/width,t=(v.pos.y+offset_y)/height;
        v.pos={corners[0].x*(1-u)*(1-t)+corners[1].x*u*(1-t)+corners[2].x*u*t+corners[3].x*(1-u)*t,
               corners[0].y*(1-u)*(1-t)+corners[1].y*u*(1-t)+corners[2].y*u*t+corners[3].y*(1-u)*t};
        const float alpha=prompt.alpha[0]*(1-u)*(1-t)+prompt.alpha[1]*u*(1-t)+prompt.alpha[2]*u*t+prompt.alpha[3]*(1-u)*t;
        ImU32 color=uint32_t(std::clamp(float(v.col>>24)*alpha,0.f,255.f))<<24;
        for(size_t lane=0;lane<3;++lane) {
            const float tint=prompt.tint[0][lane]*(1-u)*(1-t)+prompt.tint[1][lane]*u*(1-t)+prompt.tint[2][lane]*u*t+prompt.tint[3][lane]*(1-u)*t;
            color|=uint32_t(std::clamp(float((v.col>>(lane*8))&255)*tint,0.f,255.f))<<(lane*8);
        }
        v.col=color;
    }
    for(int i=first_command;i<list.CmdBuffer.Size;++i)list.CmdBuffer[i].ClipRect={left,top,right,bottom};
    list.PopClipRect();
}
}
void load_prompt_font(std::vector<char>& font_bytes) {
        wchar_t directory[32768];const auto size=GetWindowsDirectoryW(directory,32768);
        if(!size || size>=32768)throw std::runtime_error("Windows fonts directory unavailable");
        std::ifstream file(std::filesystem::path(std::wstring(directory,size))/"Fonts"/"segoeuib.ttf",std::ios::binary);
        font_bytes.assign(std::istreambuf_iterator<char>(file),{});
        if(font_bytes.empty() || font_bytes.size()>INT_MAX)throw std::runtime_error("Segoe UI font unavailable");
        ImFontConfig font_config;font_config.FontDataOwnedByAtlas=false;
        if(!ImGui::GetIO().Fonts->AddFontFromMemoryTTF(font_bytes.data(),int(font_bytes.size()),20,&font_config))throw std::runtime_error("native prompt font initialization failed");
}
void draw_prompt_picture(const gx::PromptDraw& prompt,RenderExtent extent,const std::array<uint32_t,4>& source,PromptStyle style) {
    if(!source[2] || !source[3])throw std::invalid_argument("invalid native prompt display source");
    picture(*ImGui::GetBackgroundDrawList(),prompt,extent,source,style);
}
}
