#include "graphite.h"
#include <imgui.h>

namespace uc::launcher {
namespace {
ImVec4 color(unsigned rgb, float alpha = 1.0f) {
    return ImVec4(float((rgb >> 16) & 255) / 255.0f,
                  float((rgb >> 8) & 255) / 255.0f,
                  float(rgb & 255) / 255.0f, alpha);
}
}

void graphite_style() {
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
}
