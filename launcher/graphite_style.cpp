#include "graphite.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <string>

namespace uc::launcher {
namespace {
ImVec4 color(unsigned rgb, float alpha = 1.0f) {
    return ImVec4(float((rgb >> 16) & 255) / 255.0f,
                  float((rgb >> 8) & 255) / 255.0f,
                  float(rgb & 255) / 255.0f, alpha);
}
}

void graphite_style() {
    // ScaleAllSizes runs on this style every frame: start from unscaled metrics, including the fields the theme leaves at defaults, or they grow by the scale each frame.
    ImGui::GetStyle() = ImGuiStyle{};
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(0, 0);
    style.ItemSpacing = ImVec2(14, 10);
    style.FramePadding = ImVec2(16, 11);
    style.WindowRounding = style.ChildRounding = style.FrameRounding = 6;
    style.PopupRounding = style.ScrollbarRounding = style.GrabRounding = 6;
    style.TabRounding = 6;
    style.WindowBorderSize = style.ChildBorderSize = 0;
    style.PopupBorderSize = 1;
    style.FrameBorderSize = 1;
    auto* c = style.Colors;
    const auto ivory = color(0xeee9dc);
    const auto muted = color(0x929899);
    const auto accent = color(0x9d1522);
    const auto hover = color(0xb32331);
    const auto active = color(0x7e101b);
    c[ImGuiCol_Text] = ivory;
    c[ImGuiCol_TextDisabled] = muted;
    c[ImGuiCol_WindowBg] = color(0x141819);
    c[ImGuiCol_ChildBg] = color(0x141819,0);
    c[ImGuiCol_PopupBg] = color(0x1c2122);
    c[ImGuiCol_Border] = color(0x555a5b);
    c[ImGuiCol_BorderShadow] = color(0x000000, 0);
    c[ImGuiCol_FrameBg] = color(0x15191a);
    c[ImGuiCol_FrameBgHovered] = color(0x303739);
    c[ImGuiCol_FrameBgActive] = color(0x3b4244);
    c[ImGuiCol_TitleBg] = color(0x101415);
    c[ImGuiCol_TitleBgActive] = color(0x1c2122);
    c[ImGuiCol_TitleBgCollapsed] = color(0x101415);
    c[ImGuiCol_MenuBarBg] = color(0x1c2122);
    c[ImGuiCol_ScrollbarBg] = color(0x101415);
    c[ImGuiCol_ScrollbarGrab] = color(0x3b4244);
    c[ImGuiCol_ScrollbarGrabHovered] = color(0x50585a);
    c[ImGuiCol_ScrollbarGrabActive] = muted;
    c[ImGuiCol_CheckMark] = ivory;
    c[ImGuiCol_CheckboxSelectedBg] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = hover;
    c[ImGuiCol_Button] = color(0x242a2c);
    c[ImGuiCol_ButtonHovered] = color(0x3b4244);
    c[ImGuiCol_ButtonActive] = active;
    c[ImGuiCol_Header] = color(0x9d1522, 0.45f);
    c[ImGuiCol_HeaderHovered] = color(0x9d1522, 0.70f);
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Separator] = color(0x303638);
    c[ImGuiCol_SeparatorHovered] = hover;
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = color(0x929899, 0.20f);
    c[ImGuiCol_ResizeGripHovered] = hover;
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_Tab] = color(0x242a2c);
    c[ImGuiCol_TabHovered] = hover;
    c[ImGuiCol_TabActive] = accent;
    c[ImGuiCol_TabUnfocused] = color(0x1c2122);
    c[ImGuiCol_TabUnfocusedActive] = color(0x3b4244);
    c[ImGuiCol_PlotLines] = muted;
    c[ImGuiCol_PlotLinesHovered] = hover;
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_PlotHistogramHovered] = hover;
    c[ImGuiCol_TableHeaderBg] = color(0x242a2c);
    c[ImGuiCol_TableBorderStrong] = c[ImGuiCol_Border];
    c[ImGuiCol_TableBorderLight] = color(0x2a3032);
    c[ImGuiCol_TableRowBg] = color(0x000000, 0);
    c[ImGuiCol_TableRowBgAlt] = color(0xeee9dc, 0.035f);
    c[ImGuiCol_TextSelectedBg] = color(0x9d1522, 0.55f);
    c[ImGuiCol_DragDropTarget] = ivory;
    c[ImGuiCol_NavHighlight] = hover;
    c[ImGuiCol_NavWindowingHighlight] = color(0xeee9dc, 0.70f);
    c[ImGuiCol_NavWindowingDimBg] = color(0x101415, 0.65f);
    c[ImGuiCol_ModalWindowDimBg] = color(0x101415, 0.75f);
}

bool combo(const char* id,const char* preview,float unit,int flags) {
    // BeginCombo switches to the popup window: take the parent's geometry and draw list first.
    const auto a=ImGui::GetCursorScreenPos();
    auto* list=ImGui::GetWindowDrawList();auto* font=ImGui::GetFont();const float size=ImGui::GetFontSize();
    const auto text=ImGui::GetColorU32(ImGuiCol_Text);
    const ImVec2 b{a.x+ImGui::CalcItemWidth(),a.y+ImGui::GetFrameHeight()};
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{12*unit,8*unit});
    const bool opened=ImGui::BeginCombo(id,"",flags|ImGuiComboFlags_NoArrowButton);
    ImGui::PopStyleVar();
    const ImVec4 clip{a.x+14*unit,a.y,b.x-36*unit,b.y};
    list->AddRectFilledMultiColor({a.x+2,a.y+2},{b.x-2,b.y-2},IM_COL32(255,255,255,5),IM_COL32(255,255,255,5),0,0);
    list->AddText(font,size,{std::round(a.x+16*unit),std::round(a.y+(b.y-a.y-size)/2)},text,preview,nullptr,0,&clip);
    const float x=b.x-25*unit,y=(a.y+b.y)/2;
    list->AddLine({x-6*unit,y-3*unit},{x,y+3*unit},text,unit);
    list->AddLine({x,y+3*unit},{x+6*unit,y-3*unit},text,unit);
    return opened;
}

void tooltip(const char* text) {
    const float size=ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{.55f*size,.4f*size});
    if(ImGui::BeginTooltip()) {ImGui::TextUnformatted(text);ImGui::EndTooltip();}
    ImGui::PopStyleVar();
}

bool slider_int(const char* id,int& value,int low,int high,const char* format) {
    std::string shown_format(format);
    if(const auto at=shown_format.find("%d");at!=std::string::npos) shown_format.replace(at,2,"%.0f");
    float shown=float(value);
    if(!ImGui::SliderFloat(id,&shown,float(low),float(high),shown_format.c_str(),ImGuiSliderFlags_AlwaysClamp)) return false;
    value=std::clamp(int(std::lround(shown)),low,high);
    return true;
}
}
