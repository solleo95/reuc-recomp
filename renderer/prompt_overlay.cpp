#include "prompt_overlay.h"
#include <intrin.h>
#include <SDL3/SDL.h>
#include "imgui.h"
#include "imgui_impl_vulkan.h"
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
struct Context {
    ImGuiContext* previous=ImGui::GetCurrentContext();
    explicit Context(ImGuiContext* current){ImGui::SetCurrentContext(current);}
    ~Context(){ImGui::SetCurrentContext(previous);}
};
void check(VkResult result) {if(result!=VK_SUCCESS)throw std::runtime_error("native prompt Vulkan error: "+std::to_string(result));}
struct Token {std::string label;std::optional<PhysicalInput> input;bool cap=false;bool both_directions=false;};
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
    for(const auto& alternative:description.alternatives) {
        if(compact && !rows.empty())rows.back().push_back({"/",{},false});
        else if(!rows.empty())rows.push_back({{"или",{},false}});
        auto& row=compact && !rows.empty() ? rows.back() : rows.emplace_back();
        for(const auto& choice:alternative.choices) {
            if(!row.empty() && row.back().label!="/")row.push_back({"+",{},false});
            if(choice.held && !compact)row.push_back({"Удерживать",{},false});
            for(size_t i=0;i<choice.bindings.size();++i) {
                if(i)row.push_back({"/",{},false});
                const auto& binding=choice.bindings[i];
                for(const auto& modifier:binding.modifiers) {
                    row.push_back({physical_name(modifier),modifier,true});row.push_back({"+",{},false});
                }
                const bool icon=binding.input.kind!=PhysicalKind::key;
                row.push_back({compact && icon ? "" : physical_name(binding.input),binding.input,true});
            }
        }
        if(alternative.gesture) {
            if(!row.empty() && row.back().label!="/")row.push_back({"+",{},false});
            row.push_back({*alternative.gesture==PromptGesture::shake ? "Встряхнуть мышью" : "Взмах мышью",{},true});
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
void picture(ImDrawList& list,const gx::PromptDraw& prompt,VkExtent2D extent,const std::array<uint32_t,4>& source) {
    std::array<ImVec2,4> corners;
    for(size_t i=0;i<4;++i)corners[i]={(prompt.efb_corners[i*2]-source[0])*extent.width/source[2],(prompt.efb_corners[i*2+1]-source[1])*extent.height/source[3]};
    const float width=length(corners[0],corners[1]),height=length(corners[0],corners[3]);
    if(width<1 || height<1)return;
    const auto rows=tokens(prompt.description,prompt.compact);
    const float pad=prompt.compact ? 1 : 4;
    auto* font=ImGui::GetFont();
    const float density=float(extent.height)/720;
    float font_size=prompt.compact ? height*.6f : std::min(24.f*density,std::min(width/5,height/3));
    auto has_icon=[](const Token& token){return token.cap && (!token.input || token.input->kind!=PhysicalKind::key);};
    auto token_width=[&](const Token& token) {return font->CalcTextSizeA(font_size,FLT_MAX,0,token.label.c_str()).x+(token.cap ? font_size*.8f : 0)+(has_icon(token) ? font_size : 0);};
    float used_width=0;
    auto layout=[&](bool draw) {
        used_width=0;
        float x=pad,y=pad;
        for(const auto& row:rows) {
            for(const auto& token:row) {
                const float w=token_width(token),h=font_size*1.55f;
                if(!prompt.compact && x>pad && x+w>width-pad) {x=pad;y+=h;}
                if(draw) {
                    if(token.cap) {
                        list.AddRectFilled({x,y},{x+w,y+h},IM_COL32(28,32,40,235),font_size*.22f);
                        list.AddRect({x,y},{x+w,y+h},IM_COL32(230,235,240,255),font_size*.22f,0,1.5f*density);
                    }
                    float text_x=x+(token.cap ? font_size*.4f : 0);
                    if(token.input && token.input->kind!=PhysicalKind::key) {
                        const float mx=text_x,my=y+font_size*.15f,mw=font_size*.7f,mh=font_size*1.2f;
                        list.AddRect({mx,my},{mx+mw,my+mh},IM_COL32_WHITE,font_size*.25f,0,1.2f*density);
                        if(token.input->kind==PhysicalKind::mouse && (token.input->code==1 || token.input->code==3)) {
                            const float left=mx+(token.input->code==3 ? mw/2 : 0);
                            list.AddRectFilled({left+1,my+2},{left+mw/2-1,my+mh*.45f},IM_COL32(90,190,255,255),1);
                        } else list.AddLine({mx+mw/2,my+mh*.2f},{mx+mw/2,my+mh*.55f},IM_COL32(90,190,255,255),2*density);
                        if(token.input->kind==PhysicalKind::wheel)for(int up:{1,0}) {
                            if(!token.both_directions && up!=(token.input->code==1))continue;
                            const float tip=up ? my-mh*.12f : my+mh*1.12f,base=up ? my+mh*.08f : my+mh*.92f;
                            list.AddTriangleFilled({mx+mw/2,tip},{mx+mw*.15f,base},{mx+mw*.85f,base},IM_COL32(90,190,255,255));
                        }
                        text_x+=font_size;
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
    const float left=std::clamp((prompt.clip[0]-source[0])*extent.width/source[2],0.f,float(extent.width));
    const float top=std::clamp((prompt.clip[1]-source[1])*extent.height/source[3],0.f,float(extent.height));
    const float right=std::clamp((prompt.clip[2]-source[0])*extent.width/source[2],left,float(extent.width));
    const float bottom=std::clamp((prompt.clip[3]-source[1])*extent.height/source[3],top,float(extent.height));
    // ImGui clips text on the CPU before its vertices receive the pane transform.
    list.PushClipRect({-1,-1},{width+1,height+1},false);
    const int first_command=list.CmdBuffer.Size-1;
    const int first=list.VtxBuffer.Size;
    const float content_height=layout(false);
    const float offset_x=(width-used_width)/2,offset_y=(height-content_height)/2;
    layout(true);
    for(int i=first;i<list.VtxBuffer.Size;++i) {
        auto& v=list.VtxBuffer[i];const float u=(v.pos.x+offset_x)/width,t=(v.pos.y+offset_y)/height;
        v.pos={corners[0].x*(1-u)*(1-t)+corners[1].x*u*(1-t)+corners[2].x*u*t+corners[3].x*(1-u)*t,
               corners[0].y*(1-u)*(1-t)+corners[1].y*u*(1-t)+corners[2].y*u*t+corners[3].y*(1-u)*t};
        const float alpha=prompt.alpha[0]*(1-u)*(1-t)+prompt.alpha[1]*u*(1-t)+prompt.alpha[2]*u*t+prompt.alpha[3]*(1-u)*t;
        v.col=(v.col&0x00FFFFFFu)|(uint32_t(std::clamp(float(v.col>>24)*alpha,0.f,255.f))<<24);
    }
    for(int i=first_command;i<list.CmdBuffer.Size;++i)list.CmdBuffer[i].ClipRect={left,top,right,bottom};
    list.PopClipRect();
}
}
void PromptOverlay::initialize(VkInstance instance,VkPhysicalDevice physical,VkDevice device,VkQueue queue,uint32_t family,uint32_t slots) {
    if(context_)return;
    context_=ImGui::CreateContext();Context current(context_);
    try {
        auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;
        wchar_t directory[32768];const auto size=GetWindowsDirectoryW(directory,32768);
        if(!size || size>=32768)throw std::runtime_error("Windows fonts directory unavailable");
        std::ifstream file(std::filesystem::path(std::wstring(directory,size))/"Fonts"/"segoeui.ttf",std::ios::binary);
        font_bytes_.assign(std::istreambuf_iterator<char>(file),{});
        if(font_bytes_.empty() || font_bytes_.size()>INT_MAX)throw std::runtime_error("Segoe UI font unavailable");
        ImFontConfig font_config;font_config.FontDataOwnedByAtlas=false;
        if(!io.Fonts->AddFontFromMemoryTTF(font_bytes_.data(),int(font_bytes_.size()),20,&font_config))throw std::runtime_error("native prompt font initialization failed");
        ImGui_ImplVulkan_InitInfo info{};info.ApiVersion=VK_API_VERSION_1_3;info.Instance=instance;info.PhysicalDevice=physical;info.Device=device;
        info.QueueFamily=family;info.Queue=queue;info.DescriptorPoolSize=32;info.MinImageCount=2;info.ImageCount=slots;info.UseDynamicRendering=true;info.CheckVkResultFn=check;
        auto& render=info.PipelineInfoMain.PipelineRenderingCreateInfo;
        render={VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};render.colorAttachmentCount=1;render.pColorAttachmentFormats=&format_;
        if(!ImGui_ImplVulkan_Init(&info))throw std::runtime_error("native prompt Vulkan initialization failed");
    } catch(...) {shutdown();throw;}
}
void PromptOverlay::record(VkCommandBuffer command,std::span<const gx::PromptDraw> prompts,VkExtent2D extent,const std::array<uint32_t,4>& source) {
    if(!context_ || !source[2] || !source[3])throw std::logic_error("native prompt renderer not initialized");
    Context current(context_);auto& io=ImGui::GetIO();io.DisplaySize={float(extent.width),float(extent.height)};io.DeltaTime=1.f/60;
    ImGui_ImplVulkan_NewFrame();ImGui::NewFrame();
    for(const auto& prompt:prompts)picture(*ImGui::GetBackgroundDrawList(),prompt,extent,source);
    ImGui::Render();ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),command);
}
void PromptOverlay::shutdown() {
    if(!context_)return;
    auto* previous=ImGui::GetCurrentContext();ImGui::SetCurrentContext(context_);
    if(ImGui::GetIO().BackendRendererUserData)ImGui_ImplVulkan_Shutdown();
    ImGui::DestroyContext(context_);ImGui::SetCurrentContext(previous==context_ ? nullptr : previous);
    context_=nullptr;font_bytes_.clear();
}
}
