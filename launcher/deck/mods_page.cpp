#include "mods_page.h"
#include "menu_text.h"
#include "mod_paths.h"
#include "standalone.h"
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
bool setting_row(RowCtx& rows, StandaloneMods& mods, ModsUi& ui, const std::string& id, ModSettings& settings, const SettingDecl& d, std::string_view language) {
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
            ui.open_list(label, choices, index < int(choices.size()) ? index : 0, [&mods, id, key, choices](int chosen) {
                if (auto* now = mods.settings(id); now && chosen >= 0 && chosen < int(choices.size())) now->set(key, choices[size_t(chosen)]);
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

bool draw_mods(RowCtx& rows, StandaloneMods& mods, ModsUi& ui, std::string_view language) {
    auto& registry = mods.registry();
    bool changed = false;
    note(rows, text_of(language, "intro").c_str(), ImGui::GetColorU32(ImGuiCol_TextDisabled));
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
            if (kNativeRuns && registry.wanted(id) && registry.allow_native() && !registry.native_hash(id).empty() && registry.native_hash(id) == mods.dll_hash(id)) view.native += "  ·  " + text_of(language, "confirmed");
        }
        view.on = registry.wanted(id);
        view.can_toggle = !broken;
        view.has_settings = !m.settings.empty();
        const auto r = mod_row(rows, id.c_str(), view);
        if (r.toggle && view.can_toggle) { registry.set_enabled(id, !view.on); changed = true; }
        if (r.settings) ui.settings_id = id;
        if ((r.up && i > 0 && registry.move(id, -1)) || (r.down && i + 1 < ids.size() && registry.move(id, 1))) { ui.follow_id = id; changed = true; }
        if (kNativeRuns && registry.wanted(id) && registry.allow_native() && !m.native.empty()) {   // a native mod's DLL is confirmed by hash before it runs
            const auto hash = mods.dll_hash(id);
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

bool draw_mod_settings(RowCtx& rows, StandaloneMods& mods, ModsUi& ui, std::string_view language) {
    bool changed = false;
    const auto found = std::find_if(mods.registry().entries().begin(), mods.registry().entries().end(), [&](const ModEntry& e) { return e.manifest.id == ui.settings_id; });
    if (found != mods.registry().entries().end())
        if (auto* settings = mods.settings(ui.settings_id))
            for (const auto& decl : found->manifest.settings) changed |= setting_row(rows, mods, ui, ui.settings_id, *settings, decl, language);
    if (button_row(rows, "close", text_of(language, "close").c_str())) ui.settings_id.clear();
    return changed;
}
}
