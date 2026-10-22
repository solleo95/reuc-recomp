#pragma once
#include "host.h"
#include "menu_bridge.h"
#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace uc::mods {

// The mod platform's part of the Insert menu, drawn with Dear ImGui into the menu's window: the Mods tab (manager), one tab per mod with
// settings or a drawing function, and the widgets a mod's drawing function calls (UiApi). Everything runs on the menu thread except commit(),
// which the main loop calls. Strings exist in English and Russian; other menu languages show English.
class ModMenuUi : public ModMenu, public UiApi {
public:
    using GetRegistry = std::function<ModRegistry*()>;   // null while mods are off
    using GetHost = std::function<ModHost*()>;           // null until the mods have started
    using SaveSection = std::function<void(const nlohmann::ordered_json& mods_section)>;

    ModMenuUi(GetRegistry registry, GetHost host, std::string language);
    void set_save(SaveSection save) { save_ = std::move(save); }   // the config's "mods" section writer (LiveSettings::apply_mods); may arrive after construction

    // ModMenu
    std::string manager_title() override;
    void manager_tab(float unit) override;
    std::vector<ModTab> tabs() override;
    void draw_tab(const std::string& id, float unit) override;
    void commit() override;

    // UiApi: valid only while draw_tab runs a mod's drawing function; outside it they return their input and draw nothing.
    void text(const std::string& text) override;
    bool button(const std::string& label) override;
    bool checkbox(const std::string& label, bool value) override;
    int slider_int(const std::string& label, int value, int min, int max) override;
    double slider_float(const std::string& label, double value, double min, double max) override;
    int combo(const std::string& label, int selected, const std::vector<std::string>& items) override;
    std::string input_text(const std::string& label, const std::string& value) override;
    void separator() override;
    void same_line() override;
    bool collapsing_header(const std::string& label) override;
    void tooltip(const std::string& text) override;
    std::array<float, 4> color_edit(const std::string& label, std::array<float, 4> rgba) override;

    bool restart_required();   // what the player changed in the manager differs from what is loaded now

    // Screen rectangle {min x, min y, max x, max y} of a control as last drawn, for tests that click it. Keys: "manager/allow_native", "manager/enable/<id>",
    // "manager/confirm/<id>", "popup/native_ok", "popup/confirm_ok", "setting/<id>/<key>", "ui/<id>/<label>" (buttons).
    std::optional<std::array<float, 4>> rect_of(const std::string& key) const;

private:
    const char* tr(const char* key) const;
    std::string localized(const std::map<std::string, std::string>& texts, const std::string& fallback) const;
    void draw_declared_settings(const std::string& id, ModSettings& settings, const std::vector<SettingDecl>& declared);
    void mark_changed();   // the registry changed: save it, refresh the conflict list
    void remember(const std::string& key);   // records the rectangle of the item just drawn (rect_of)
    const ModEntry* entry(const std::string& id) const;

    GetRegistry registry_;
    GetHost host_;
    SaveSection save_;
    std::string language_;
    bool in_draw_ = false;
    bool dirty_ = false;
    int active_frame_ = -1000;   // the last ImGui frame in which an item of a mod tab was active (commit() waits for the edit to end)
    bool open_confirm_ = false;
    std::vector<Conflict> conflicts_;
    std::vector<std::string> loaded_active_;                       // ids active when the game started, in load order
    bool loaded_allow_native_ = false;
    std::map<std::string, std::string> loaded_native_hash_;        // id -> confirmed hash at start
    std::map<std::string, std::string> hash_cache_;                // id -> sha256 of its DLL (computed once)
    std::map<std::string, nlohmann::ordered_json> saved_settings_; // the settings of each mod that changed this session, as last written to the config
    std::map<std::string, std::string> text_buffers_, last_text_;  // input_text state per (mod, label): the edit buffer and what the script was given last
    std::map<std::string, std::array<float, 4>> rects_;
    std::string current_mod_;
    std::string confirm_id_;                                      // the mod whose native code the open popup asks about
};

}  // namespace uc::mods
