#include "rows.h"
#include "input_config.h"
#include "panel_widgets.h"
#include <algorithm>
#include <cmath>
#include <vector>
namespace uc::deck {
namespace {
constexpr uint32_t kAccent = 0x9d1522, kAccentHot = 0xb32331, kIvory = 0xeee9dc, kMuted = 0x929899, kSide = 0x101415, kRow = 0x1c2122, kRowHot = 0x2b3234, kTrack = 0x3b4244;
constexpr uint32_t kDefaultReticle[2] = {0xFF3030, 0x3264FF};
ImU32 rgb(uint32_t value, float alpha = 1) { return IM_COL32((value >> 16) & 255, (value >> 8) & 255, value & 255, int(std::clamp(alpha, 0.f, 1.f) * 255 + .5f)); }
void text_at(ImDrawList& draw, float size, ImVec2 at, ImU32 color, const char* value) { draw.AddText(ImGui::GetFont(), size, at, color, value); }
float text_width(float size, const char* value) { return ImGui::GetFont()->CalcTextSizeA(size, 1e9f, 0, value).x; }
void right_text(ImDrawList& draw, float size, float right, float y, ImU32 color, const char* value) { text_at(draw, size, {std::round(right - text_width(size, value)), y}, color, value); }

struct RowFrame {
    ImVec2 at{}, end{};
    float height = 0;
    bool focused = false, hovered = false, tapped = false;
    RowKeys keys;
};
// The invisible button of a row (the hit area of a tap or a finger), the registration in the focus list and the scroll that keeps the focused row in view.
RowFrame begin_row(RowCtx& c, RowKind kind, bool enabled, float hit_width, float height) {
    const float s = c.unit;
    RowFrame f;
    f.height = height;
    f.at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x - 8 * s;
    f.end = {f.at.x + width, f.at.y + height};
    ImGui::InvisibleButton("##row", {hit_width > 0 ? hit_width : width, height});
    f.hovered = ImGui::IsItemHovered();
    if (c.touch) {
        auto& io = ImGui::GetIO();
        if (ImGui::IsItemActivated()) { c.touch->press_y = io.MousePos.y; c.touch->dragged = false; }
        if (ImGui::IsItemActive() && std::abs(io.MousePos.y - c.touch->press_y) > 10 * s) { c.touch->dragged = true; ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y); }   // a finger drags the list
        f.tapped = ImGui::IsItemDeactivated() && !c.touch->dragged && f.hovered && enabled;
    } else f.tapped = ImGui::IsItemClicked() && enabled;
    f.keys = c.focus->add(kind, enabled, &f.focused);
    if (f.tapped) c.focus->set_focus(int(c.focus->count()) - 1);   // the focus follows a tap
    // The focus also follows a moving pointer, so a row is never both focused and "hovered": a pointer that rests on a row while the D-pad moves on would light a second one.
    const auto& io = ImGui::GetIO();
    const bool followed = f.hovered && enabled && !f.focused && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0);
    if (followed) { c.focus->set_focus(int(c.focus->count()) - 1); f.focused = true; }
    if (f.focused && c.focus->moved() && !followed) ImGui::SetScrollHereY(.5f);
    f.hovered = false;
    return f;
}
void row_background(const RowFrame& f, float s, bool lit, bool accent = false, bool outline = false) {
    auto& draw = *ImGui::GetWindowDrawList();
    const ImU32 base = accent ? rgb(kAccent, .85f) : rgb(kRow, .92f);
    const ImU32 fill = f.focused ? (accent ? rgb(kAccentHot) : rgb(kAccent, .42f)) : lit ? rgb(accent ? kAccentHot : kRowHot) : base;
    draw.AddRectFilled(f.at, f.end, fill, 10 * s);
    if (f.focused) {   // a red outline does not show on the red button: its focus is a lighter fill and an ivory outline
        if (accent) draw.AddRect(f.at, f.end, rgb(kIvory), 10 * s, 0, 3.5f * s);
        else draw.AddRect(f.at, f.end, rgb(kAccentHot), 10 * s, 0, 2 * s);
    }
    else if (outline) draw.AddRect(f.at, f.end, rgb(kTrack, .7f), 10 * s, 0, 1.5f * s);   // buttons and list entries sit on the same dark colour as a modal: they need an edge
}
void row_gap(RowCtx& c, const RowFrame& f) {
    ImGui::SetCursorScreenPos({f.at.x, f.at.y + f.height});
    ImGui::Dummy({f.end.x - f.at.x, 8 * c.unit});
}
}

bool registry_row(RowCtx& c, const settings::Item& item, Options& options) {
    const float s = c.unit;
    const settings::State state = item.state ? item.state(options, c.env) : settings::State{};
    if (!state.visible || item.kind == settings::Kind::custom) return false;
    const bool slider = item.kind == settings::Kind::slider, enabled = state.enabled;
    const float bar_width = slider ? 240 * s : 0, value_width = slider ? 96 * s : 0;
    const std::string label = c.text(item.label_key);
    const float width = ImGui::GetContentRegionAvail().x - 8 * s, inner = width - 44 * s;   // 22 units of padding on each side
    constexpr float label_size = 26, value_size = 25;
    // What does not fit is wrapped, never drawn over its neighbour: a long label wraps beside the control; a choice whose label and value together are too wide puts
    // the value on a second line under the label.
    std::string shown;
    if (item.kind == settings::Kind::choice) {
        const auto labels = item.labels(options, c.env, c.text);
        const int index = item.index(options, c.env);
        shown = index >= 0 && index < int(labels.size()) ? labels[size_t(index)] : "-";
    }
    float control = item.kind == settings::Kind::toggle ? 72 * s + 24 * s : slider ? bar_width + value_width + 56 * s : item.kind == settings::Kind::color ? 60 * s + 24 * s : 0;
    const bool stacked = item.kind == settings::Kind::choice && text_width(label_size * s, label.c_str()) + text_width(value_size * s, shown.c_str()) + 36 * s + 28 * s > inner;
    float row_height = 66 * s, label_height = 0, value_height = 0;
    const auto measure = [&](float size, float wrap, const std::string& text) { return ImGui::GetFont()->CalcTextSizeA(size * s, 1e9f, wrap, text.c_str()).y; };
    if (stacked) {
        label_height = measure(label_size, inner, label); value_height = measure(value_size, inner - 36 * s, shown);
        row_height = std::max(66 * s, 16 * s + label_height + 6 * s + value_height + 16 * s);
    } else {
        label_height = measure(label_size, inner - control, label);
        row_height = std::max(66 * s, label_height + 30 * s);
    }
    if (!enabled && state.why) row_height += 24 * s;
    bool changed = false;
    ImGui::PushID(item.id);
    const RowKind kind = item.kind == settings::Kind::toggle ? RowKind::toggle : item.kind == settings::Kind::choice ? RowKind::choice : item.kind == settings::Kind::slider ? RowKind::slider : RowKind::color;
    const auto f = begin_row(c, kind, enabled, slider ? width - (bar_width + value_width + 44 * s) : 0, row_height);
    if (enabled) {
        if (f.keys.delta) changed |= settings::step(item, options, c.env, f.keys.delta);
        if (f.keys.accept || f.tapped) {
            if (item.kind == settings::Kind::toggle) changed |= settings::flip(item, options);
            else if (item.kind == settings::Kind::choice) { if (c.open_list) c.open_list(item); }
            else if (item.kind == settings::Kind::color) {   // the presets go round
                changed |= settings::step(item, options, c.env, 1);
                if (!changed) { item.set_color(options, settings::color_presets()[0]); changed = true; }
            }
        }
    }
    float bar_x = 0;
    bool bar_active = false;
    const float mid = f.at.y + (row_height - (!enabled && state.why ? 24 * s : 0)) / 2;
    if (slider) {   // the bar and its value sit at the right of the row; a finger on the bar sets the value
        bar_x = f.end.x - value_width - bar_width - 30 * s;
        ImGui::SetCursorScreenPos({bar_x - 14 * s, f.at.y});
        ImGui::InvisibleButton("##bar", {bar_width + 28 * s, row_height});
        bar_active = ImGui::IsItemActive();
        if (bar_active && enabled) {
            const float t = std::clamp((ImGui::GetIO().MousePos.x - bar_x) / bar_width, 0.f, 1.f);
            c.focus->set_focus(int(c.focus->count()) - 1);
            changed |= settings::set_slider(item, options, item.min + int(std::lround(t * (item.max - item.min))));
        }
    }
    auto& draw = *ImGui::GetWindowDrawList();
    row_background(f, s, f.hovered || bar_active);
    const ImU32 ink = rgb(enabled ? kIvory : kMuted, enabled ? 1.f : .75f);
    const float label_wrap = stacked ? inner : inner - control;
    const float label_y = stacked ? f.at.y + 16 * s : mid - label_height / 2;
    draw.AddText(ImGui::GetFont(), label_size * s, {f.at.x + 22 * s, std::round(label_y)}, ink, label.c_str(), nullptr, label_wrap);
    if (!enabled && state.why) text_at(draw, 19 * s, {f.at.x + 22 * s, f.end.y - 30 * s}, rgb(kMuted), c.text(state.why).c_str());
    const float edge = f.end.x - 22 * s, fade = enabled ? 1.f : .5f;
    switch (item.kind) {
    case settings::Kind::toggle: {   // a switch
        const bool on = item.get_bool(options);
        const ImVec2 a{edge - 72 * s, mid - 18 * s}, b{edge, mid + 18 * s};
        draw.AddRectFilled(a, b, on ? rgb(kAccent, fade) : rgb(kTrack, fade), 18 * s);
        draw.AddCircleFilled({on ? b.x - 18 * s : a.x + 18 * s, mid}, 14 * s, rgb(kIvory, fade), 24);
        break;
    }
    case settings::Kind::choice: {   // the value and a chevron
        const float chevron_x = edge - 12 * s;
        const float cy = stacked ? f.at.y + 16 * s + label_height + 6 * s + value_height / 2 : mid;
        if (stacked) draw.AddText(ImGui::GetFont(), value_size * s, {f.at.x + 22 * s, std::round(f.at.y + 16 * s + label_height + 6 * s)}, rgb(kMuted), shown.c_str(), nullptr, inner - 36 * s);
        else right_text(draw, value_size * s, edge - 36 * s, std::round(mid - value_size * s / 2), ink, shown.c_str());
        draw.AddLine({chevron_x - 8 * s, cy - 4 * s}, {chevron_x, cy + 5 * s}, ink, 2.2f * s); draw.AddLine({chevron_x, cy + 5 * s}, {chevron_x + 8 * s, cy - 4 * s}, ink, 2.2f * s);
        break;
    }
    case settings::Kind::slider: {
        const float t = item.max > item.min ? float(item.get_int(options) - item.min) / float(item.max - item.min) : 0;
        draw.AddRectFilled({bar_x, mid - 5 * s}, {bar_x + bar_width, mid + 5 * s}, rgb(kTrack, fade), 5 * s);
        draw.AddRectFilled({bar_x, mid - 5 * s}, {bar_x + bar_width * t, mid + 5 * s}, rgb(kAccentHot, fade), 5 * s);
        draw.AddCircleFilled({bar_x + bar_width * t, mid}, 16 * s, rgb(kIvory, fade), 24);
        right_text(draw, 25 * s, edge, std::round(mid - 12.5f * s), ink, settings::slider_text(item, options).c_str());
        break;
    }
    case settings::Kind::color: {
        const uint32_t value = item.get_color(options);
        const uint32_t shown_colour = value ? value : kDefaultReticle[std::string(item.id).find("p2") != std::string::npos ? 1 : 0];
        const ImVec2 a{edge - 60 * s, mid - 17 * s}, b{edge, mid + 17 * s};
        draw.AddRectFilled(a, b, rgb(shown_colour), 8 * s); draw.AddRect(a, b, rgb(kIvory, .6f), 8 * s, 0, 1.5f * s);
        if (!value) right_text(draw, 23 * s, a.x - 14 * s, std::round(mid - 11.5f * s), rgb(kMuted), c.text("reticle_default").c_str());
        break;
    }
    case settings::Kind::custom: break;
    }
    ImGui::PopID();
    row_gap(c, f);
    return changed;
}

bool button_row(RowCtx& c, const char* id, const char* label, bool enabled, bool primary) {
    const float s = c.unit;
    ImGui::PushID(id);
    const auto f = begin_row(c, RowKind::button, enabled, 0, (primary ? 78 : 66) * s);
    auto& draw = *ImGui::GetWindowDrawList();
    row_background(f, s, f.hovered && enabled, primary, !primary);
    const float size = (primary ? 30 : 26) * s;
    ImFont* font = primary && c.serif ? c.serif : ImGui::GetFont();
    const ImVec2 extent = font->CalcTextSizeA(size, 1e9f, 0, label);
    draw.AddText(font, size, {std::round(f.at.x + (f.end.x - f.at.x - extent.x) / 2), std::round(f.at.y + (f.height - size) / 2)}, rgb(enabled ? kIvory : kMuted, enabled ? 1.f : .6f), label);
    ImGui::PopID();
    row_gap(c, f);
    return enabled && (f.keys.accept || f.tapped);
}

bool entry_row(RowCtx& c, const char* id, const std::string& label, EntryMark mark) {
    const float s = c.unit;
    ImGui::PushID(id);
    const auto f = begin_row(c, RowKind::button, true, 0, 62 * s);
    auto& draw = *ImGui::GetWindowDrawList();
    row_background(f, s, f.hovered, false, true);
    const float mid = f.at.y + f.height / 2;
    draw.PushClipRect({f.at.x, f.at.y}, {f.end.x - 56 * s, f.end.y}, true);
    text_at(draw, 26 * s, {f.at.x + 22 * s, std::round(mid - 13 * s)}, rgb(kIvory), label.c_str());
    draw.PopClipRect();
    if (mark == EntryMark::current) draw.AddCircleFilled({f.end.x - 34 * s, mid}, 8 * s, rgb(kAccentHot), 20);
    else if (mark == EntryMark::folder) { draw.AddLine({f.end.x - 38 * s, mid - 9 * s}, {f.end.x - 28 * s, mid}, rgb(kMuted), 2.4f * s); draw.AddLine({f.end.x - 28 * s, mid}, {f.end.x - 38 * s, mid + 9 * s}, rgb(kMuted), 2.4f * s); }
    ImGui::PopID();
    row_gap(c, f);
    return f.keys.accept || f.tapped;
}

void binding_row(RowCtx& c, BindingEditor& editor, Action action, PadFamily family) {
    const float s = c.unit, small = 48 * s, tools = 2 * small + 24 * s;
    ImGui::PushID(int(action));
    auto& io = ImGui::GetIO(); (void)io;
    const auto f = begin_row(c, RowKind::binding, true, ImGui::GetContentRegionAvail().x - 8 * s - tools, 68 * s);
    // The row has three places for the D-pad: the row itself (A replaces the bindings: the next pad button is bound), the plus and the cross. Left/right move between
    // them; A acts on the one that is selected; X and Y add and clear from anywhere in the row.
    int own_sub = 0;
    int& sub = c.sub ? *c.sub : own_sub;
    if (f.focused && c.focus->moved()) sub = 0;   // a row that was just reached starts on the row itself
    if (f.focused && f.keys.delta) sub = std::clamp(sub + f.keys.delta, 0, 2);
    if (f.tapped) editor.begin_capture(action, true);
    else if (f.keys.accept) { if (sub == 1) editor.begin_capture(action); else if (sub == 2) editor.clear(action); else editor.begin_capture(action, true); }
    if (f.keys.add) editor.begin_capture(action);
    if (f.keys.clear) editor.clear(action);
    const float top = f.at.y + (f.height - small) / 2;
    ImGui::SetCursorScreenPos({f.end.x - 2 * small - 16 * s, top});
    ImGui::InvisibleButton("##add", {small, small});
    const bool add_hot = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) { c.focus->set_focus(int(c.focus->count()) - 1); editor.begin_capture(action); }
    const ImVec2 add_at = ImGui::GetItemRectMin();
    ImGui::SetCursorScreenPos({f.end.x - small - 8 * s, top});
    ImGui::InvisibleButton("##clear", {small, small});
    const bool clear_hot = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) { c.focus->set_focus(int(c.focus->count()) - 1); editor.clear(action); }
    const ImVec2 clear_at = ImGui::GetItemRectMin();
    auto& draw = *ImGui::GetWindowDrawList();
    row_background(f, s, f.hovered);
    text_at(draw, 26 * s, {f.at.x + 22 * s, std::round(f.at.y + (f.height - 26 * s) / 2)}, rgb(kIvory), c.panel_text(action_names[size_t(action)]));
    const float width = f.end.x - f.at.x;
    const ImVec2 low{f.at.x + width * .34f, f.at.y + 8 * s}, high{f.end.x - tools - 6 * s, f.end.y - 8 * s};
    if (editor.capturing() == action) {
        const char* hint = c.panel_text("deck_press_button");
        right_text(draw, 25 * s, (low.x + high.x + text_width(25 * s, hint)) / 2, std::round((low.y + high.y) / 2 - 12.5f * s), rgb(kIvory), hint);
    } else panel_shared::draw_pieces(draw, low, high, panel_shared::pieces(editor.bindings()[size_t(action)], c.panel_text), family, c.panel_text("unbound"));
    const float k = 9 * s;   // the add button's plus and the clear button's cross
    const bool add_selected = f.focused && sub == 1, clear_selected = f.focused && sub == 2;
    const auto icon_background = [&](ImVec2 at, bool hot, bool selected) {
        draw.AddRectFilled(at, {at.x + small, at.y + small}, selected ? rgb(kAccent) : hot ? rgb(kTrack) : rgb(kSide), small / 2);
        if (selected) draw.AddCircle({at.x + small / 2, at.y + small / 2}, small / 2 + 3 * s, rgb(kAccentHot), 32, 2.5f * s);
    };
    icon_background(add_at, add_hot, add_selected);
    const ImVec2 plus{add_at.x + small / 2, add_at.y + small / 2};
    draw.AddLine({plus.x - k, plus.y}, {plus.x + k, plus.y}, rgb(kIvory), 2.6f * s); draw.AddLine({plus.x, plus.y - k}, {plus.x, plus.y + k}, rgb(kIvory), 2.6f * s);
    icon_background(clear_at, clear_hot, clear_selected);
    const ImVec2 cross{clear_at.x + small / 2, clear_at.y + small / 2};
    const float m = 8 * s;
    draw.AddLine({cross.x - m, cross.y - m}, {cross.x + m, cross.y + m}, rgb(kIvory), 2.6f * s); draw.AddLine({cross.x - m, cross.y + m}, {cross.x + m, cross.y - m}, rgb(kIvory), 2.6f * s);
    ImGui::PopID();
    row_gap(c, f);
}

void hold_row(RowCtx& c, const char* label, PhysicalInput current, PadFamily family) {
    const float s = c.unit;
    ImGui::PushID("gyro_hold_button");
    const auto f = begin_row(c, RowKind::button, true, 0, 68 * s);
    const bool waiting = c.hold_capture && *c.hold_capture;
    if ((f.keys.accept || f.tapped) && c.hold_capture) *c.hold_capture = true;
    auto& draw = *ImGui::GetWindowDrawList();
    row_background(f, s, f.hovered);
    text_at(draw, 26 * s, {f.at.x + 22 * s, std::round(f.at.y + (f.height - 26 * s) / 2)}, rgb(kIvory), label);
    const float width = f.end.x - f.at.x, tools = 2 * 48 * s + 24 * s;   // the picture sits where a binding row's does (binding_row), whose plus and cross this row has no use for
    const ImVec2 low{f.at.x + width * .34f, f.at.y + 8 * s}, high{f.end.x - tools - 6 * s, f.end.y - 8 * s};
    if (waiting) {
        const char* hint = c.panel_text("deck_press_button");
        right_text(draw, 25 * s, (low.x + high.x + text_width(25 * s, hint)) / 2, std::round((low.y + high.y) / 2 - 12.5f * s), rgb(kIvory), hint);
    } else panel_shared::draw_pieces(draw, low, high, {panel_shared::Piece{{}, current}}, family, c.panel_text("unbound"));
    ImGui::PopID();
    row_gap(c, f);
}
ModRowResult mod_row(RowCtx& c, const char* id, const ModRowView& v) {
    const float s = c.unit, small = 46 * s, gap = 8 * s, tools = 3 * small + 2 * gap;
    ImGui::PushID(id);
    const float width = ImGui::GetContentRegionAvail().x - 8 * s;
    const float text_x = 22 * s + 72 * s + 22 * s;   // after the switch
    const float wrap = width - text_x - tools - 30 * s;
    constexpr float title_size = 26, line_size = 21;
    const auto measure = [&](float size, const std::string& text) { return text.empty() ? 0.f : ImGui::GetFont()->CalcTextSizeA(size * s, 1e9f, wrap, text.c_str()).y + 4 * s; };
    const float body = measure(title_size, v.title) + measure(line_size, v.status) + measure(line_size, v.description) + measure(line_size, v.problems) + measure(line_size, v.native);
    const float height = std::max(84 * s, body + 28 * s);
    const auto f = begin_row(c, RowKind::binding, true, width - tools - 16 * s, height);
    // The parts the D-pad can select, in a fixed order whatever the mod (so that a mod that moved keeps the selected button): the row (A switches the mod), the gear
    // (only a mod with settings), load earlier, load later.
    enum Part { row, gear, up, down };
    std::vector<Part> parts{row};
    if (v.has_settings) parts.push_back(gear);
    parts.push_back(up); parts.push_back(down);
    int own_sub = 0;
    int& sub = c.sub ? *c.sub : own_sub;
    if (f.focused && c.focus->moved()) sub = 0;
    if (f.focused) {
        if (f.keys.delta) sub += f.keys.delta;
        sub = std::clamp(sub, 0, int(parts.size()) - 1);
    }
    const Part active = parts[size_t(f.focused ? sub : 0)];
    ModRowResult result;
    result.toggle = f.tapped || (f.keys.accept && active == row);
    if (f.keys.accept) { result.settings = active == gear; result.up = active == up; result.down = active == down; }
    const float top = f.at.y + (f.height - small) / 2, x0 = f.end.x - tools - 10 * s;
    ImVec2 tool_at[3];
    for (int i = 0; i < 3; ++i) {   // gear, up, down: columns that do not move from mod to mod
        ImGui::SetCursorScreenPos({x0 + float(i) * (small + gap), top});
        ImGui::InvisibleButton(i == 0 ? "##gear" : i == 1 ? "##up" : "##down", {small, small});
        tool_at[i] = ImGui::GetItemRectMin();
        if (ImGui::IsItemClicked() && (i != 0 || v.has_settings)) {
            c.focus->set_focus(int(c.focus->count()) - 1);
            if (i == 0) result.settings = true; else if (i == 1) result.up = true; else result.down = true;
        }
    }
    auto& draw = *ImGui::GetWindowDrawList();
    row_background(f, s, false);
    const float mid = f.at.y + height / 2, fade = v.can_toggle ? 1.f : .5f;
    const ImVec2 a{f.at.x + 22 * s, mid - 18 * s}, b{a.x + 72 * s, mid + 18 * s};
    draw.AddRectFilled(a, b, v.on ? rgb(kAccent, fade) : rgb(kTrack, fade), 18 * s);
    draw.AddCircleFilled({v.on ? b.x - 18 * s : a.x + 18 * s, mid}, 14 * s, rgb(kIvory, fade), 24);
    float y = f.at.y + 14 * s;
    const auto line = [&](float size, const std::string& text, ImU32 color) {
        if (text.empty()) return;
        draw.AddText(ImGui::GetFont(), size * s, {f.at.x + text_x, std::round(y)}, color, text.c_str(), nullptr, wrap);
        y += ImGui::GetFont()->CalcTextSizeA(size * s, 1e9f, wrap, text.c_str()).y + 4 * s;
    };
    line(title_size, v.title, rgb(kIvory, fade));
    line(line_size, v.status, rgb(kMuted));
    line(line_size, v.description, rgb(kIvory));
    line(line_size, v.problems, IM_COL32(217, 115, 102, 255));
    line(line_size, v.native, rgb(kMuted));
    const float k = 8 * s;
    for (int i = 0; i < 3; ++i) {
        if (i == 0 && !v.has_settings) continue;
        const bool selected = f.focused && active == (i == 0 ? gear : i == 1 ? up : down);
        const ImVec2 at = tool_at[i], centre{at.x + small / 2, at.y + small / 2};
        draw.AddRectFilled(at, {at.x + small, at.y + small}, selected ? rgb(kAccent) : rgb(kSide), small / 2);
        if (selected) draw.AddCircle(centre, small / 2 + 3 * s, rgb(kAccentHot), 32, 2.5f * s);
        if (i == 0) {   // a gear: a ring with eight teeth
            draw.AddCircle(centre, small * .19f, rgb(kIvory), 20, small * .09f);
            for (int tooth = 0; tooth < 8; ++tooth) {
                const float angle = float(tooth) * 0.7853981634f, dx = std::cos(angle), dy = std::sin(angle);
                draw.AddLine({centre.x + dx * small * .21f, centre.y + dy * small * .21f}, {centre.x + dx * small * .33f, centre.y + dy * small * .33f}, rgb(kIvory), small * .12f);
            }
        } else if (i == 1) draw.AddTriangleFilled({centre.x, centre.y - k}, {centre.x - k * 1.2f, centre.y + k * .7f}, {centre.x + k * 1.2f, centre.y + k * .7f}, rgb(kIvory));
        else draw.AddTriangleFilled({centre.x, centre.y + k}, {centre.x - k * 1.2f, centre.y - k * .7f}, {centre.x + k * 1.2f, centre.y - k * .7f}, rgb(kIvory));
    }
    ImGui::PopID();
    row_gap(c, f);
    return result;
}

namespace {
// `text` cut to `width` at a character boundary, with an ellipsis (a value from a mod can be any length).
std::string fit_text(float size, const std::string& text, float width) {
    if (text_width(size, text.c_str()) <= width) return text;
    std::string cut = text;
    while (!cut.empty()) {
        while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80) cut.pop_back();   // whole UTF-8 characters only: the continuation bytes, then the lead byte
        if (!cut.empty()) cut.pop_back();
        if (text_width(size, (cut + "…").c_str()) <= width) break;
    }
    return cut + "…";
}
}

ValueResult value_row(RowCtx& c, const char* id, const std::string& label, const ValueView& v) {
    const float s = c.unit;
    const bool slider = v.kind == ValueView::Kind::slider;
    const float bar_width = 240 * s, value_width = 96 * s;
    const float width = ImGui::GetContentRegionAvail().x - 8 * s, inner = width - 44 * s;
    const float control = v.kind == ValueView::Kind::toggle ? 96 * s : slider ? bar_width + value_width + 56 * s : std::min(inner * .4f, 360 * s) + 36 * s;
    const float label_height = ImGui::GetFont()->CalcTextSizeA(26 * s, 1e9f, inner - control, label.c_str()).y;
    const float height = std::max(66 * s, label_height + 30 * s);
    ImGui::PushID(id);
    const RowKind kind = v.kind == ValueView::Kind::toggle ? RowKind::toggle : slider ? RowKind::slider : RowKind::choice;
    const auto f = begin_row(c, kind, true, slider ? width - (bar_width + value_width + 44 * s) : 0, height);
    ValueResult result;
    result.delta = f.keys.delta;
    result.activated = f.keys.accept || f.tapped;
    const float mid = f.at.y + height / 2, edge = f.end.x - 22 * s;
    float bar_x = 0;
    bool bar_active = false;
    if (slider) {   // a finger or the pointer on the bar sets the value
        bar_x = f.end.x - value_width - bar_width - 30 * s;
        ImGui::SetCursorScreenPos({bar_x - 14 * s, f.at.y});
        ImGui::InvisibleButton("##bar", {bar_width + 28 * s, height});
        bar_active = ImGui::IsItemActive();
        if (bar_active) {
            c.focus->set_focus(int(c.focus->count()) - 1);
            result.set_t = std::clamp((ImGui::GetIO().MousePos.x - bar_x) / bar_width, 0.f, 1.f);
        }
    }
    auto& draw = *ImGui::GetWindowDrawList();
    row_background(f, s, bar_active);
    draw.AddText(ImGui::GetFont(), 26 * s, {f.at.x + 22 * s, std::round(mid - label_height / 2)}, rgb(kIvory), label.c_str(), nullptr, inner - control);
    switch (v.kind) {
    case ValueView::Kind::toggle: {
        const ImVec2 a{edge - 72 * s, mid - 18 * s}, b{edge, mid + 18 * s};
        draw.AddRectFilled(a, b, v.on ? rgb(kAccent) : rgb(kTrack), 18 * s);
        draw.AddCircleFilled({v.on ? b.x - 18 * s : a.x + 18 * s, mid}, 14 * s, rgb(kIvory), 24);
        break;
    }
    case ValueView::Kind::slider:
        draw.AddRectFilled({bar_x, mid - 5 * s}, {bar_x + bar_width, mid + 5 * s}, rgb(kTrack), 5 * s);
        draw.AddRectFilled({bar_x, mid - 5 * s}, {bar_x + bar_width * v.t, mid + 5 * s}, rgb(kAccentHot), 5 * s);
        draw.AddCircleFilled({bar_x + bar_width * v.t, mid}, 16 * s, rgb(kIvory), 24);
        right_text(draw, 25 * s, edge, std::round(mid - 12.5f * s), rgb(kIvory), v.shown.c_str());
        break;
    case ValueView::Kind::text: {
        const float chevron_x = edge - 12 * s;
        right_text(draw, 25 * s, edge - 36 * s, std::round(mid - 12.5f * s), rgb(kIvory), fit_text(25 * s, v.shown, control - 36 * s).c_str());
        draw.AddLine({chevron_x - 8 * s, mid - 4 * s}, {chevron_x, mid + 5 * s}, rgb(kIvory), 2.2f * s); draw.AddLine({chevron_x, mid + 5 * s}, {chevron_x + 8 * s, mid - 4 * s}, rgb(kIvory), 2.2f * s);
        break;
    }
    }
    ImGui::PopID();
    row_gap(c, f);
    return result;
}

void caption(RowCtx& c, const char* label) {
    ImGui::Dummy({0, 8 * c.unit});
    if (c.serif) ImGui::PushFont(c.serif, std::round(30 * c.unit));
    ImGui::TextUnformatted(label);
    if (c.serif) ImGui::PopFont();
    ImGui::Dummy({0, 6 * c.unit});
}
void note(RowCtx& c, const char* label, ImU32 color) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", label);
    ImGui::PopStyleColor();
    ImGui::Dummy({0, 8 * c.unit});
}
}
