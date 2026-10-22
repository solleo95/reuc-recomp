#include "menu_ui.h"
#include "mod_paths.h"
#include "runtime_native.h"
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace uc::mods {
namespace {
struct Text { const char* key; const char* en; const char* ru; };
constexpr Text kTexts[] = {
    {"mods_tab", "Mods", "Моды"},
    {"intro", "Mods live in the mods folder next to the game. Enabling, disabling and reordering mods, and changes to files, textures and native code, apply the next time the game starts.",
              "Моды лежат в папке mods рядом с игрой. Включение, выключение и порядок модов, а также файлы, текстуры и нативный код вступают в силу при следующем запуске игры."},
    {"none", "No mods found. Put a mod folder (or .zip) into the mods folder next to the game.", "Модов нет. Положите папку мода (или .zip) в папку mods рядом с игрой."},
    {"restart", "Restart the game to apply the changes.", "Перезапустите игру, чтобы применить изменения."},
    {"allow_native", "Allow native (DLL) mods", "Разрешить нативные (DLL) моды"},
    {"native_title", "Native mods", "Нативные моды"},
    {"native_warning", "A native mod runs with full access to your computer, like any program. Only enable them from sources you trust. Each DLL must also be confirmed separately.",
                       "Нативный мод работает с полным доступом к компьютеру, как любая программа. Включайте их только из источников, которым доверяете. Каждую DLL нужно подтвердить отдельно."},
    {"ok", "Enable", "Включить"},
    {"cancel", "Cancel", "Отмена"},
    {"reload", "Reload scripts", "Перезагрузить скрипты"},
    {"enabled", "working", "работает"},
    {"disabled", "off", "выключен"},
    {"error", "error", "ошибка"},
    {"incompatible", "incompatible", "несовместим"},
    {"missing", "dependency missing", "нет зависимости"},
    {"dep_off", "dependency is off", "зависимость выключена"},
    {"stopped", "switched off for this session", "отключён до перезапуска"},
    {"has_native", "contains native code", "содержит нативный код"},
    {"gameplay_note", "Changes how the game plays: the game uses the separate modded saves while this mod is on.", "Меняет игровой процесс: пока мод включён, игра использует отдельные сохранения для модов."},
    {"confirmed", "native code confirmed", "нативный код подтверждён"},
    {"confirm", "Confirm native code…", "Подтвердить нативный код…"},
    {"confirm_title", "Confirm native code", "Подтверждение нативного кода"},
    {"confirm_text", "This DLL can do anything on your computer. Confirm only if you trust where the mod comes from. The file's SHA-256 is:", "Эта DLL может делать на компьютере что угодно. Подтверждайте, только если доверяете источнику мода. SHA-256 файла:"},
    {"conflicts", "Files provided by more than one mod (the last mod in the list wins):", "Файлы, которые дают несколько модов (побеждает нижний в списке):"},
    {"up", "Load earlier", "Грузить раньше"},
    {"down", "Load later", "Грузить позже"},
    {"restart_setting", "(restart needed)", "(нужен перезапуск)"},
    {"drawing_failed", "This mod's tab failed:", "Вкладка мода дала сбой:"},
};
}  // namespace

ModMenuUi::ModMenuUi(GetRegistry registry, GetHost host, std::string language) : registry_(std::move(registry)), host_(std::move(host)), language_(std::move(language)) {
    if (auto* r = registry_()) {
        for (const auto* mod : r->active()) loaded_active_.push_back(mod->manifest.id);
        loaded_allow_native_ = r->allow_native();
        for (const auto& e : r->entries()) loaded_native_hash_[e.manifest.id] = r->native_hash(e.manifest.id);
        conflicts_ = r->conflicts();
    }
}

const char* ModMenuUi::tr(const char* key) const {
    for (const auto& t : kTexts) if (std::strcmp(t.key, key) == 0) return language_ == "ru" ? t.ru : t.en;
    return key;
}

std::string ModMenuUi::localized(const std::map<std::string, std::string>& texts, const std::string& fallback) const {
    if (const auto at = texts.find(language_); at != texts.end()) return at->second;
    if (const auto at = texts.find("en"); at != texts.end()) return at->second;
    return fallback;
}

const ModEntry* ModMenuUi::entry(const std::string& id) const {
    if (auto* r = registry_()) for (const auto& e : r->entries()) if (e.manifest.id == id) return &e;
    return nullptr;
}

std::string ModMenuUi::manager_title() { return tr("mods_tab"); }

void ModMenuUi::remember(const std::string& key) {
    const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    rects_[key] = {lo.x, lo.y, hi.x, hi.y};
}

std::optional<std::array<float, 4>> ModMenuUi::rect_of(const std::string& key) const {
    const auto at = rects_.find(key);
    if (at == rects_.end()) return std::nullopt;
    return at->second;
}

bool ModMenuUi::restart_required() {
    auto* r = registry_();
    if (!r) return false;
    std::vector<std::string> now;
    for (const auto* mod : r->active()) now.push_back(mod->manifest.id);
    if (now != loaded_active_ || r->allow_native() != loaded_allow_native_) return true;
    for (const auto& id : now) if (loaded_native_hash_[id] != r->native_hash(id)) return true;
    return false;
}

void ModMenuUi::mark_changed() {
    dirty_ = true;
    if (auto* r = registry_()) conflicts_ = r->conflicts();
}

void ModMenuUi::manager_tab(float unit) {
    auto* r = registry_();
    ImGui::Dummy({0, 6 * unit});
    ImGui::TextWrapped("%s", tr("intro"));
    if (!r) { ImGui::Spacing(); ImGui::TextWrapped("%s", tr("none")); return; }
    if (restart_required()) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, .78f, .35f, 1.f)); ImGui::TextWrapped("%s", tr("restart")); ImGui::PopStyleColor(); }
    ImGui::Separator();

    bool allow = r->allow_native();
    if (ImGui::Checkbox(tr("allow_native"), &allow)) {
        if (allow) ImGui::OpenPopup("##native_warning");
        else { r->set_allow_native(false); mark_changed(); }
    }
    remember("manager/allow_native");
    if (ImGui::BeginPopupModal("##native_warning", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
        ImGui::TextUnformatted(tr("native_title"));
        ImGui::Separator();
        ImGui::TextWrapped("%s", tr("native_warning"));
        ImGui::PopTextWrapPos();
        const bool enable_native = ImGui::Button(tr("ok"));
        remember("popup/native_ok");
        if (enable_native) { r->set_allow_native(true); mark_changed(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button(tr("cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (auto* host = host_()) {
        ImGui::SameLine();
        if (ImGui::Button(tr("reload"))) host->reload_scripts();
    }
    ImGui::Separator();

    const auto status = host_() ? host_()->status() : std::vector<ModHost::ModStatus>();
    // Copy the ids: moving a mod reorders the list under the loop.
    std::vector<std::string> ids;
    for (const auto& e : r->entries()) ids.push_back(e.manifest.id);
    if (ids.empty()) ImGui::TextWrapped("%s", tr("none"));
    int index = 0;
    for (const auto& id : ids) {
        const ModEntry* found = entry(id);
        if (!found) continue;
        const ModEntry copy = *found;   // moving or toggling a mod below reorders the registry's list: this frame keeps drawing the row it started with
        const ModEntry* e = &copy;
        ImGui::PushID(index++);
        const bool broken = e->state == ModState::error && !e->manifest.version.size();   // a manifest that did not parse has no version
        bool on = r->wanted(id);
        const std::string title = localized(e->manifest.names, e->manifest.name) + (e->manifest.version.empty() ? "" : "  v" + e->manifest.version);
        ImGui::BeginDisabled(broken);
        if (ImGui::Checkbox(title.c_str(), &on)) { r->set_enabled(id, on); mark_changed(); }
        remember("manager/enable/" + id);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("^")) { r->move(id, -1); mark_changed(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("up"));
        ImGui::SameLine();
        if (ImGui::SmallButton("v")) { r->move(id, 1); mark_changed(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("down"));
        const char* badge = e->state == ModState::enabled ? "enabled" : e->state == ModState::disabled ? "disabled" : e->state == ModState::incompatible ? "incompatible"
            : e->state == ModState::missing_dependency ? "missing" : e->state == ModState::dependency_disabled ? "dep_off" : "error";
        std::string line = tr(badge);
        if (!e->manifest.author.empty()) line += "  ·  " + e->manifest.author;
        const auto running = std::find_if(status.begin(), status.end(), [&](const ModHost::ModStatus& s) { return s.id == id; });
        if (running != status.end() && running->disabled_for_session) line = std::string(tr("stopped")) + ": " + running->last_error;
        ImGui::TextDisabled("%s", line.c_str());
        if (!e->manifest.description.empty()) ImGui::TextWrapped("%s", e->manifest.description.c_str());
        if (e->manifest.touches_gameplay) ImGui::TextDisabled("%s", tr("gameplay_note"));
        for (const auto& message : e->messages) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, .52f, .52f, 1.f)); ImGui::TextWrapped("%s", message.c_str()); ImGui::PopStyleColor(); }
        if (!e->manifest.native.empty()) {
            ImGui::TextDisabled("%s", tr("has_native"));
            if (r->wanted(id) && r->allow_native()) {
                auto& hash = hash_cache_[id];
                if (hash.empty()) if (const auto dll = confine(e->root, e->manifest.native)) hash = sha256_file_hex(*dll);
                if (!hash.empty() && r->native_hash(id) == hash) { ImGui::SameLine(); ImGui::TextDisabled("· %s", tr("confirmed")); }
                else {
                    const bool asked = ImGui::SmallButton(tr("confirm"));
                    remember("manager/confirm/" + id);
                    if (asked) { confirm_id_ = id; open_confirm_ = true; }   // opened after the loop: a popup's id depends on the id stack it is opened in
                }
            }
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    if (open_confirm_) { ImGui::OpenPopup("##native_confirm"); open_confirm_ = false; }
    if (ImGui::BeginPopupModal("##native_confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
        ImGui::TextUnformatted(tr("confirm_title"));
        ImGui::Separator();
        ImGui::TextWrapped("%s", tr("confirm_text"));
        ImGui::TextUnformatted(hash_cache_[confirm_id_].c_str());
        ImGui::PopTextWrapPos();
        const bool confirmed_now = ImGui::Button(tr("ok"));
        remember("popup/confirm_ok");
        if (confirmed_now) { r->set_native_hash(confirm_id_, hash_cache_[confirm_id_]); mark_changed(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button(tr("cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (!conflicts_.empty()) {
        ImGui::TextWrapped("%s", tr("conflicts"));
        for (const auto& c : conflicts_) {
            std::string who;
            for (const auto& id : c.mod_ids) who += (who.empty() ? "" : ", ") + id;
            ImGui::TextDisabled("%s  (%s)", c.path.c_str(), who.c_str());
        }
    }
}

std::vector<ModTab> ModMenuUi::tabs() {
    std::vector<ModTab> result;
    auto* host = host_();
    auto* r = registry_();
    if (!host || !r) return result;
    const auto status = host->status();
    for (const auto& e : r->entries()) {
        const auto it = std::find_if(status.begin(), status.end(), [&](const ModHost::ModStatus& s) { return s.id == e.manifest.id; });
        if (it == status.end() || !it->running || it->disabled_for_session || !(it->has_ui || it->has_settings)) continue;
        result.push_back({e.manifest.id, localized(e.manifest.names, e.manifest.name)});
    }
    return result;
}

void ModMenuUi::draw_declared_settings(const std::string&, ModSettings& settings, const std::vector<SettingDecl>& declared) {
    for (const auto& d : declared) {
        ImGui::PushID(d.key.c_str());
        const std::string label = localized(d.label, d.key) + (d.restart_required ? std::string("  ") + tr("restart_setting") : std::string());
        const auto current = settings.get(d.key);
        switch (d.type) {
            case SettingType::boolean: {
                bool v = current.is_boolean() && current.get<bool>();
                if (ImGui::Checkbox(label.c_str(), &v)) settings.set(d.key, v);
                break;
            }
            case SettingType::integer: {
                int v = current.is_number() ? int(current.get<long long>()) : 0;
                const bool changed = d.min && d.max ? ImGui::SliderInt(label.c_str(), &v, int(*d.min), int(*d.max)) : ImGui::DragInt(label.c_str(), &v);
                if (changed) settings.set(d.key, v);
                break;
            }
            case SettingType::number: {
                float v = current.is_number() ? float(current.get<double>()) : 0.f;
                const bool changed = d.min && d.max ? ImGui::SliderFloat(label.c_str(), &v, float(*d.min), float(*d.max), "%.3g") : ImGui::DragFloat(label.c_str(), &v, d.step ? float(*d.step) : 0.1f);
                if (changed) settings.set(d.key, double(v));
                break;
            }
            case SettingType::choice: {
                const std::string now = current.is_string() ? current.get<std::string>() : std::string();
                if (ImGui::BeginCombo(label.c_str(), now.c_str())) {
                    for (const auto& choice : d.choices) if (ImGui::Selectable(choice.c_str(), choice == now)) settings.set(d.key, choice);
                    ImGui::EndCombo();
                }
                break;
            }
            case SettingType::text: {
                std::string v = current.is_string() ? current.get<std::string>() : std::string();
                if (ImGui::InputText(label.c_str(), &v)) settings.set(d.key, v);
                break;
            }
        }
        remember("setting/" + current_mod_ + "/" + d.key);
        if (ImGui::IsItemHovered() && !d.tooltip.empty()) ImGui::SetTooltip("%s", localized(d.tooltip, "").c_str());
        ImGui::PopID();
    }
}

void ModMenuUi::draw_tab(const std::string& id, float unit) {
    auto* host = host_();
    if (!host) return;
    ImGui::PushID(id.c_str());
    current_mod_ = id;
    ImGui::Dummy({0, 6 * unit});
    const auto declared = entry(id) ? entry(id)->manifest.settings : std::vector<SettingDecl>();
    if (!declared.empty()) host->edit_settings(id, [&](ModSettings& settings) { draw_declared_settings(id, settings, declared); });
    const auto status = host->status();
    const auto it = std::find_if(status.begin(), status.end(), [&](const ModHost::ModStatus& s) { return s.id == id; });
    if (it != status.end() && it->has_ui) {
        if (!declared.empty()) ImGui::Separator();
        in_draw_ = true;
        ImGuiHandles imgui;
        imgui.context = ImGui::GetCurrentContext();
        ImGui::GetAllocatorFunctions(&imgui.alloc, &imgui.free, &imgui.user);
        const CallResult result = host->draw_ui(id, &imgui);
        in_draw_ = false;
        if (!result.ok) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, .52f, .52f, 1.f));
            ImGui::TextWrapped("%s %s", tr("drawing_failed"), result.error.c_str());
            ImGui::PopStyleColor();
        }
    }
    if (ImGui::IsAnyItemActive()) active_frame_ = ImGui::GetFrameCount();   // a slider is being dragged or a text field edited
    current_mod_.clear();
    ImGui::PopID();
}

void ModMenuUi::commit() {
    auto* r = registry_();
    if (!r) return;
    auto* host = host_();
    if (!host && !dirty_) return;   // runs every main-loop iteration: with no running mod and nothing changed in the manager it must cost nothing
    // While a widget of a mod's tab is being edited the value changes every frame: write it (and tell the mod) once, when the edit ends.
    if (ImGui::GetCurrentContext() && ImGui::GetFrameCount() - active_frame_ <= 1) return;
    bool changed = dirty_;
    std::vector<std::pair<std::string, std::string>> notify;   // (mod id, setting key) to tell the mods about, after the locks are released
    if (host) {
        for (const auto& id : host->ids()) {
            nlohmann::ordered_json now;
            std::vector<std::string> keys;
            bool dirty = false;
            // Never wait for a mod here (this runs on the main loop): a busy mod is looked at again next frame, its changes stay marked.
            host->try_edit_settings(id, [&](ModSettings& settings) {
                dirty = settings.dirty();
                if (dirty) now = settings.to_json();
                keys = settings.take_changed();
                settings.clear_dirty();
            });
            if (dirty) { saved_settings_[id] = now; changed = true; }   // a mod's settings are written whole once any of them changed
            for (const auto& key : keys) notify.emplace_back(id, key);
        }
    }
    if (changed && save_) {
        nlohmann::ordered_json section = r->to_config();
        for (const auto& [id, settings] : saved_settings_) {
            if (!section.contains(id) || !section[id].is_object()) section[id] = nlohmann::ordered_json::object();
            section[id]["settings"] = settings;
        }
        save_(section);
    }
    dirty_ = false;
    if (host) for (const auto& [id, key] : notify) host->settings_changed(id, key);
}

// ---- UiApi ----
void ModMenuUi::text(const std::string& value) { if (in_draw_) ImGui::TextWrapped("%s", value.c_str()); }
bool ModMenuUi::button(const std::string& label) {
    if (!in_draw_) return false;
    const bool pressed = ImGui::Button(label.c_str());
    remember("ui/" + current_mod_ + "/" + label);
    return pressed;
}
bool ModMenuUi::checkbox(const std::string& label, bool value) {
    if (in_draw_) ImGui::Checkbox(label.c_str(), &value);
    return value;
}
int ModMenuUi::slider_int(const std::string& label, int value, int min, int max) {
    if (in_draw_) ImGui::SliderInt(label.c_str(), &value, min, max);
    return value;
}
double ModMenuUi::slider_float(const std::string& label, double value, double min, double max) {
    if (in_draw_) { float v = float(value); ImGui::SliderFloat(label.c_str(), &v, float(min), float(max)); value = v; }
    return value;
}
int ModMenuUi::combo(const std::string& label, int selected, const std::vector<std::string>& items) {
    if (!in_draw_ || items.empty()) return selected;
    selected = std::clamp(selected, 0, int(items.size()) - 1);
    if (ImGui::BeginCombo(label.c_str(), items[size_t(selected)].c_str())) {
        for (size_t i = 0; i < items.size(); ++i) if (ImGui::Selectable(items[i].c_str(), int(i) == selected)) selected = int(i);
        ImGui::EndCombo();
    }
    return selected;
}
std::string ModMenuUi::input_text(const std::string& label, const std::string& value) {
    if (!in_draw_) return value;
    const std::string key = current_mod_ + "/" + label;
    auto& buffer = text_buffers_[key];
    auto& last = last_text_[key];
    if (value != last) buffer = value;   // the script passed a value other than the one it got last time: it changed the text itself, follow it
    ImGui::InputText(label.c_str(), &buffer);
    if (buffer.size() > 1024) buffer.resize(1024);
    last = buffer;
    return buffer;
}
void ModMenuUi::separator() { if (in_draw_) ImGui::Separator(); }
void ModMenuUi::same_line() { if (in_draw_) ImGui::SameLine(); }
bool ModMenuUi::collapsing_header(const std::string& label) { return in_draw_ && ImGui::CollapsingHeader(label.c_str()); }
void ModMenuUi::tooltip(const std::string& value) { if (in_draw_ && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", value.c_str()); }
std::array<float, 4> ModMenuUi::color_edit(const std::string& label, std::array<float, 4> rgba) {
    if (in_draw_) ImGui::ColorEdit4(label.c_str(), rgba.data());
    return rgba;
}

}  // namespace uc::mods
