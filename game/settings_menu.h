#pragma once
#include "live_settings.h"
#include "feature_report.h"
#include "binding_editor.h"
#include "ui_fonts.h"
#include <functional>
#include <set>
#include <string>
union SDL_Event;
namespace uc {
// The in-game settings menu (Insert): Main (render scale, upscaling, quality, FPS limit, frame generation, bloom) and Controls (prompt style,
// bindings), laid out like the launcher. Every pick goes through LiveSettings, which applies and saves it.
class SettingsMenu {
public:
    // features: the PC's DLSS/FSR support once known (nullptr while checking); monitor: the desktop size for the render scale labels.
    SettingsMenu(LiveSettings& live, std::string language, std::function<const FeatureReport*()> features, uint32_t monitor_width, uint32_t monitor_height);
    void setup();   // renderer set_menu setup: fonts and style into the menu's ImGui context
    void build();   // renderer set_menu build: one frame of widgets; runs inside the renderer, so picks wait for commit()
    void commit();  // main loop, outside rendering: apply and save what build() picked
    bool capturing() const { return editor_.capturing().has_value(); }
    void capture_event(const SDL_Event&);   // key/mouse events while a binding is being captured
    void focus_lost() { editor_.focus(false); }
    void upscaler_failed(UpscalerKind kind) { failed_.insert(kind); }   // could not start in this process: greyed out, changed in the launcher
private:
    const char* tr(std::string_view key) const;
    void main_tab(float unit);
    void controls_tab(float unit);
    const Options& current() const { return pending_ ? *pending_ : live_.options(); }
    LiveSettings& live_;
    std::optional<Options> pending_;   // picked by build(), applied by commit()
    std::string language_;
    std::function<const FeatureReport*()> features_;
    uint32_t monitor_width_, monitor_height_;
    BindingEditor editor_;
    ui::Fonts fonts_;
    std::set<UpscalerKind> failed_;
};
}
