#include "mods_page.h"
#include "menu_text.h"
#include "mod_paths.h"
#include "standalone.h"
#include "imgui_stdlib.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
namespace uc::deck {
namespace {
using namespace uc::mods;
#if defined(__linux__)
constexpr bool kNativeRuns = false;   // the game cannot load a native DLL on Linux: no switch for it, no confirmation (uc::mods::make_native_runtime)
#else
constexpr bool kNativeRuns = true;
#endif
std::string text_of(std::string_view language, const char* key) {   // the manager's own words, as in the in-game menu
    const char* text = manager_text(language, key);
    return text ? text : key;
}
std::string localized(const std::map<std::string, std::string>& texts, const std::string& fallback, std::string_view language) {
    if (const auto at = texts.find(std::string(language)); at != texts.end()) return at->second;
    if (const auto at = texts.find("en"); at != texts.end()) return at->second;
    return fallback;
}
std::string number_text(double value, bool integer) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, integer ? "%.0f" : "%.6g", value);
    return buffer;
}
// One declared setting as a row; returns true when the value changed.
bool setting_row(RowCtx& rows, ModsAccess& access, ModsUi& ui, const std::string& id, ModSettings& settings, const SettingDecl& d, std::string_view language) {
    const std::string label = localized(d.label, d.key, language) + (d.restart_required ? "  " + text_of(language, "restart_setting") : std::string());
    const nlohmann::json current = settings.get(d.key);
    switch (d.type) {
    case SettingType::boolean: {
        const bool on = current.is_boolean() && current.get<bool>();
        const auto r = value_row(rows, d.key.c_str(), label, {ValueView::Kind::toggle, on, 0, {}});
        const bool want = r.delta ? r.delta > 0 : r.activated ? !on : on;
        return want != on && settings.set(d.key, want);
    }
    case SettingType::integer: case SettingType::number: {
        const bool integer = d.type == SettingType::integer;
        const double value = current.is_number() ? current.get<double>() : 0;
        const bool bounded = d.min && d.max && *d.max > *d.min;
        const double step = d.step && *d.step > 0 ? *d.step : bounded && !integer ? (*d.max - *d.min) / 100 : integer ? 1 : .1;
        ValueView view{bounded ? ValueView::Kind::slider : ValueView::Kind::text, false, bounded ? float((value - *d.min) / (*d.max - *d.min)) : 0.f, number_text(value, integer)};
        const auto r = value_row(rows, d.key.c_str(), label, view);
        double next = value;
        if (r.delta) next = value + r.delta * step;
        else if (r.set_t >= 0 && bounded) next = *d.min + double(r.set_t) * (*d.max - *d.min);
        if (next == value) return false;
        const nlohmann::json json = integer ? nlohmann::json(static_cast<long long>(std::llround(next))) : nlohmann::json(next);
        const auto before = settings.get(d.key);
        return settings.set(d.key, json) && settings.get(d.key) != before;   // a value at the end of its range is not a change
    }
    case SettingType::choice: {
        const std::string now = current.is_string() ? current.get<std::string>() : std::string();
        const int index = int(std::find(d.choices.begin(), d.choices.end(), now) - d.choices.begin());
        const auto r = value_row(rows, d.key.c_str(), label, {ValueView::Kind::text, false, 0, now});
        if (r.delta) {
            const int next = std::clamp(index + r.delta, 0, int(d.choices.size()) - 1);
            return next != index && settings.set(d.key, d.choices[size_t(next)]);
        }
        if (r.activated && ui.open_list) {
            const auto choices = d.choices;
            const auto key = d.key;
            ui.open_list(label, choices, index < int(choices.size()) ? index : 0, [access, id, key, choices](int chosen) {
                if (chosen >= 0 && chosen < int(choices.size())) access.edit(id, [&](ModSettings& now) { now.set(key, choices[size_t(chosen)]); });
            });
        }
        return false;
    }
    case SettingType::text: {
        const std::string now = current.is_string() ? current.get<std::string>() : std::string();
        const auto r = value_row(rows, d.key.c_str(), label, {ValueView::Kind::text, false, 0, now});
        if (r.activated) { ui.text_open = true; ui.text_key = d.key; ui.text_buffer = now; }
        return false;
    }
    }
    return false;
}
}

ModsAccess access_of(StandaloneMods& mods) {
    return {[&mods] { return &mods.registry(); },
            [&mods](const std::string& id, const std::function<void(ModSettings&)>& edit) { auto* settings = mods.settings(id); if (settings) edit(*settings); return settings != nullptr; },
            [&mods](const std::string& id) { return mods.dll_hash(id); }};
}
bool draw_mods(RowCtx& rows, StandaloneMods& mods, ModsUi& ui, std::string_view language) { auto access = access_of(mods); return draw_mods(rows, access, ui, language); }
bool draw_mod_settings(RowCtx& rows, StandaloneMods& mods, ModsUi& ui, std::string_view language) { auto access = access_of(mods); return draw_mod_settings(rows, access, ui, language); }

bool draw_mods(RowCtx& rows, ModsAccess& access, ModsUi& ui, std::string_view language) {
    bool changed = false;
    note(rows, text_of(language, "intro").c_str(), ImGui::GetColorU32(ImGuiCol_TextDisabled));
    auto* live = access.registry();
    if (!live) { note(rows, text_of(language, "none").c_str(), ImGui::GetColorU32(ImGuiCol_TextDisabled)); return false; }
    auto& registry = *live;
    if (kNativeRuns) {
        const bool allow = registry.allow_native();
        const auto r = value_row(rows, "allow_native", text_of(language, "allow_native"), {ValueView::Kind::toggle, allow, 0, {}});
        const bool want = r.delta ? r.delta > 0 : r.activated ? !allow : allow;
        if (want && !allow) ui.native_warning = true;   // switching it on is asked about first
        else if (!want && allow) { registry.set_allow_native(false); changed = true; }
    }
    std::vector<std::string> ids;
    for (const auto& entry : registry.entries()) ids.push_back(entry.manifest.id);
    if (ids.empty()) note(rows, text_of(language, "none").c_str(), ImGui::GetColorU32(ImGuiCol_TextDisabled));
    for (size_t i = 0; i < ids.size(); ++i) {
        const auto& id = ids[i];
        const auto found = std::find_if(registry.entries().begin(), registry.entries().end(), [&](const ModEntry& e) { return e.manifest.id == id; });
        if (found == registry.entries().end()) continue;
        const ModEntry entry = *found;   // a move below reorders the registry: this frame keeps the row it started with
        if (ui.follow_id == id) { rows.focus->follow(int(rows.focus->count())); ui.follow_id.clear(); }   // the mod that moved: the focus goes with it
        const auto& m = entry.manifest;
        const bool broken = entry.state == ModState::error && m.version.empty();   // a manifest that did not parse has no version
        const char* badge = entry.state == ModState::enabled ? "enabled" : entry.state == ModState::disabled ? "disabled" : entry.state == ModState::incompatible ? "incompatible"
            : entry.state == ModState::missing_dependency ? "missing" : entry.state == ModState::dependency_disabled ? "dep_off" : "error";
        ModRowView view;
        view.title = localized(m.names, m.name, language) + (m.version.empty() ? "" : "  v" + m.version);
        view.status = text_of(language, badge);
        if (!m.author.empty()) view.status += "  ·  " + m.author;
        if (kNativeRuns && entry.state == ModState::enabled && !m.native.empty() && !registry.allow_native()) view.status = text_of(language, "waiting");   // switched on, but native code is off
        view.description = m.description;
        for (const auto& message : entry.messages) view.problems += (view.problems.empty() ? "" : "\n") + message;
        if (!m.native.empty()) {
            view.native = text_of(language, kNativeRuns ? "has_native" : "native_linux");
            if (kNativeRuns && registry.wanted(id) && registry.allow_native() && !registry.native_hash(id).empty() && registry.native_hash(id) == access.dll_hash(id)) view.native += "  ·  " + text_of(language, "confirmed");
        }
        view.on = registry.wanted(id);
        view.can_toggle = !broken;
        view.has_settings = !m.settings.empty();
        const auto r = mod_row(rows, id.c_str(), view);
        if (r.toggle && view.can_toggle) { registry.set_enabled(id, !view.on); changed = true; }
        if (r.settings) ui.settings_id = id;
        if ((r.up && i > 0 && registry.move(id, -1)) || (r.down && i + 1 < ids.size() && registry.move(id, 1))) { ui.follow_id = id; changed = true; }
        if (kNativeRuns && registry.wanted(id) && registry.allow_native() && !m.native.empty()) {   // a native mod's DLL is confirmed by hash before it runs
            const auto hash = access.dll_hash(id);
            if (hash.empty() || registry.native_hash(id) != hash)
                if (button_row(rows, ("confirm:" + id).c_str(), text_of(language, "confirm").c_str())) ui.confirm_id = id;
        }
    }
    if (changed) ui.conflicts_known = false;
    if (!ui.conflicts_known) {
        ui.conflict_lines.clear();
        for (const auto& conflict : registry.conflicts()) {
            std::string who;
            for (const auto& id : conflict.mod_ids) who += (who.empty() ? "" : ", ") + id;
            ui.conflict_lines.push_back(conflict.path + "  (" + who + ")");
        }
        ui.conflicts_known = true;
    }
    if (!ui.conflict_lines.empty()) {
        note(rows, text_of(language, "conflicts").c_str(), ImGui::GetColorU32(ImGuiCol_Text));
        for (const auto& line : ui.conflict_lines) note(rows, line.c_str(), ImGui::GetColorU32(ImGuiCol_TextDisabled));
    }
    return changed;
}

bool draw_mod_settings(RowCtx& rows, ModsAccess& access, ModsUi& ui, std::string_view language) {
    bool changed = false;
    if (auto* registry = access.registry()) {
        const auto found = std::find_if(registry->entries().begin(), registry->entries().end(), [&](const ModEntry& e) { return e.manifest.id == ui.settings_id; });
        if (found != registry->entries().end()) {
            const auto declared = found->manifest.settings;   // the edit below may reorder nothing, but a copy keeps the loop independent of the registry
            const std::string id = ui.settings_id;
            access.edit(id, [&](ModSettings& settings) { for (const auto& decl : declared) changed |= setting_row(rows, access, ui, id, settings, decl, language); });
        }
    }
    if (button_row(rows, "close", text_of(language, "close").c_str())) ui.settings_id.clear();
    return changed;
}

bool mods_window_open(const ModsUi& ui) { return !ui.settings_id.empty() || ui.native_warning || !ui.confirm_id.empty() || ui.text_open || ui.list.open; }

namespace {
// A choice's list, drawn inside the settings window (ImGui keeps one popup per level: a popup beside it would close it).
void draw_list_window(ModsUi& ui, ModsWindows& w, std::span<const DeckKey> keys) {
    const auto display = ImGui::GetIO().DisplaySize;
    const float unit = w.unit;
    if (!ImGui::IsPopupOpen("###mod_list")) ImGui::OpenPopup("###mod_list");
    ImGui::SetNextWindowSize({std::min(720 * unit, display.x - 32 * unit), std::min(640 * unit, display.y - 32 * unit)}, ImGuiCond_Always);
    ImGui::SetNextWindowPos({display.x / 2, display.y / 2}, ImGuiCond_Always, {.5f, .5f});
    if (!ImGui::BeginPopupModal("###mod_list", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav)) return;
    w.modal_focus->begin_frame(keys);
    RowCtx rows = w.make_rows(*w.modal_focus);
    int chosen = -1;
    ImGui::PushFont(w.serif, std::round(34 * unit)); ImGui::TextUnformatted(ui.list.title.c_str()); ImGui::PopFont();
    ImGui::Dummy({0, 6 * unit});
    ImGui::BeginChild("entries", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
    for (int i = 0; i < int(ui.list.labels.size()); ++i)
        if (entry_row(rows, std::to_string(i).c_str(), ui.list.labels[size_t(i)], i == ui.list.current ? EntryMark::current : EntryMark::none)) chosen = i;
    ImGui::EndChild();
    w.modal_focus->end_frame();
    const bool back = std::find(keys.begin(), keys.end(), DeckKey::back) != keys.end();
    if (chosen >= 0) { auto choose = ui.list.choose; ui.list.open = false; ImGui::CloseCurrentPopup(); if (choose) choose(chosen); if (w.settings_changed) w.settings_changed(); }
    else if (back) { ui.list.open = false; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

// The entry of a text setting, inside the settings window. B is left to the text field: on a keyboard it is Backspace.
void draw_text_window(ModsAccess& access, ModsUi& ui, ModsWindows& w, std::span<const DeckKey> keys) {
    const auto display = ImGui::GetIO().DisplaySize;
    const float unit = w.unit;
    const bool fresh = !ImGui::IsPopupOpen("###mod_text");
    if (fresh) { ImGui::OpenPopup("###mod_text"); w.modal_focus->set_focus(0); }
    ImGui::SetNextWindowSize({std::min(900 * unit, display.x - 32 * unit), 0}, ImGuiCond_Always);
    ImGui::SetNextWindowPos({display.x / 2, display.y / 2}, ImGuiCond_Always, {.5f, .5f});
    if (!ImGui::BeginPopupModal("###mod_text", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav)) return;
    w.modal_focus->begin_frame(keys);
    RowCtx rows = w.make_rows(*w.modal_focus);
    ImGui::PushFont(w.serif, std::round(34 * unit)); ImGui::TextUnformatted(ui.text_key.c_str()); ImGui::PopFont();
    ImGui::Dummy({0, 4 * unit});
    if (fresh) ImGui::SetKeyboardFocusHere();   // typing starts at once (and asks the system for its keyboard)
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##text", &ui.text_buffer);
    ImGui::Dummy({0, 8 * unit});
    const bool done = button_row(rows, "done", text_of(w.language, "close").c_str());   // keeps the text
    const bool cancel = button_row(rows, "cancel", text_of(w.language, "cancel").c_str());
    w.modal_focus->end_frame();
    if (done) {   // a value the mod refuses (over 1024 bytes) leaves the old one
        bool set = false;
        access.edit(ui.settings_id, [&](ModSettings& settings) { set = settings.set(ui.text_key, ui.text_buffer); });
        if (set && w.settings_changed) w.settings_changed();
    }
    if (done || cancel) { ui.text_open = false; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}
}

void draw_mods_windows(ModsAccess& access, ModsUi& ui, ModsWindows& w, std::span<const DeckKey> keys) {
    auto* registry = access.registry();
    if (!registry) return;
    const float unit = w.unit;
    const auto display = ImGui::GetIO().DisplaySize;
    const auto& language = w.language;
    const bool back = std::find(keys.begin(), keys.end(), DeckKey::back) != keys.end();
    const auto centre = [&](float width) {
        ImGui::SetNextWindowSize({std::min(width * unit, display.x - 32 * unit), 0}, ImGuiCond_Always);
        ImGui::SetNextWindowPos({display.x / 2, display.y / 2}, ImGuiCond_Always, {.5f, .5f});
    };
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {24 * unit, 22 * unit});
    // Which layer takes this frame's keys is decided once, from what was open when the frame started: a window opened during the frame gets its keys from the next one,
    // and the key that opened it is not also given to the window below.
    const bool list_was_open = ui.list.open, text_was_open = ui.text_open;
    if (!ui.settings_id.empty()) {
        if (!ImGui::IsPopupOpen("###mod_settings")) { ImGui::OpenPopup("###mod_settings"); w.settings_focus->set_focus(0); }
        ImGui::SetNextWindowSize({std::min(900 * unit, display.x - 32 * unit), std::min(640 * unit, display.y - 32 * unit)}, ImGuiCond_Always);
        ImGui::SetNextWindowPos({display.x / 2, display.y / 2}, ImGuiCond_Always, {.5f, .5f});
        if (ImGui::BeginPopupModal("###mod_settings", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav)) {
            const bool top = !list_was_open && !text_was_open;   // a list or the text entry over this window has the keys
            w.settings_focus->begin_frame(top ? keys : std::span<const DeckKey>{});
            RowCtx rows = w.make_rows(*w.settings_focus);
            std::string title = ui.settings_id;
            for (const auto& entry : registry->entries())
                if (entry.manifest.id == ui.settings_id) { const auto at = entry.manifest.names.find(language); title = at != entry.manifest.names.end() ? at->second : entry.manifest.name; }
            ImGui::PushFont(w.serif, std::round(34 * unit)); ImGui::TextUnformatted(title.c_str()); ImGui::PopFont();
            ImGui::Dummy({0, 6 * unit});
            ImGui::BeginChild("mod-settings-rows", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
            const bool changed = draw_mod_settings(rows, access, ui, language);
            ImGui::EndChild();
            w.settings_focus->end_frame();
            if (changed && w.settings_changed) w.settings_changed();
            if (ui.list.open) draw_list_window(ui, w, list_was_open ? keys : std::span<const DeckKey>{});
            if (ui.text_open) draw_text_window(access, ui, w, text_was_open ? keys : std::span<const DeckKey>{});
            if (ui.settings_id.empty() || (top && back)) { ui.settings_id.clear(); ui.text_open = false; ui.list.open = false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
    }
    if (ui.native_warning) {
        if (!ImGui::IsPopupOpen("###native_warning")) { ImGui::OpenPopup("###native_warning"); w.modal_focus->set_focus(1); }
        centre(760);
        if (ImGui::BeginPopupModal("###native_warning", nullptr, kFlags)) {
            w.modal_focus->begin_frame(keys);
            RowCtx rows = w.make_rows(*w.modal_focus);
            ImGui::PushFont(w.serif, std::round(34 * unit)); ImGui::TextUnformatted(text_of(language, "native_title").c_str()); ImGui::PopFont();
            ImGui::Dummy({0, 4 * unit});
            note(rows, text_of(language, "native_warning").c_str(), ImGui::GetColorU32(ImGuiCol_Text));
            const bool allow = button_row(rows, "allow", text_of(language, "ok").c_str());
            const bool cancel = button_row(rows, "cancel", text_of(language, "cancel").c_str());
            w.modal_focus->end_frame();
            if (allow) { registry->set_allow_native(true); ui.conflicts_known = false; if (w.registry_changed) w.registry_changed(); }
            if (allow || cancel || back) { ui.native_warning = false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
    }
    if (!ui.confirm_id.empty()) {
        if (!ImGui::IsPopupOpen("###native_confirm")) { ImGui::OpenPopup("###native_confirm"); w.modal_focus->set_focus(1); }
        centre(900);
        if (ImGui::BeginPopupModal("###native_confirm", nullptr, kFlags)) {
            w.modal_focus->begin_frame(keys);
            RowCtx rows = w.make_rows(*w.modal_focus);
            const std::string hash = access.dll_hash(ui.confirm_id);
            ImGui::PushFont(w.serif, std::round(34 * unit)); ImGui::TextUnformatted(text_of(language, "confirm_title").c_str()); ImGui::PopFont();
            ImGui::Dummy({0, 4 * unit});
            note(rows, text_of(language, "confirm_text").c_str(), ImGui::GetColorU32(ImGuiCol_Text));
            note(rows, hash.c_str(), ImGui::GetColorU32(ImGuiCol_TextDisabled));
            const bool confirmed = button_row(rows, "confirm", text_of(language, "ok").c_str(), !hash.empty());
            const bool cancel = button_row(rows, "cancel", text_of(language, "cancel").c_str());
            w.modal_focus->end_frame();
            if (confirmed) { registry->set_native_hash(ui.confirm_id, hash); if (w.registry_changed) w.registry_changed(); }
            if (confirmed || cancel || back) { ui.confirm_id.clear(); ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
    }
    ImGui::PopStyleVar();
}
}
