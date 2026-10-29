#pragma once
#include "live_settings.h"
#include "feature_report.h"
#include "backend_ui.h"
#include "binding_editor.h"
#include "gamepad_panel.h"
#include "ui_fonts.h"
#include "menu_bridge.h"
#include <functional>
#include <set>
#include <string>
union SDL_Event;
namespace uc {
// The in-game settings menu (Insert): Main (render scale, upscaling, quality, RCAS, FPS limit, frame generation, bloom) and Controls (prompt style,
// bindings), laid out like the launcher. Every pick goes through LiveSettings, which applies and saves it.
class SettingsMenu {
public:
    // features: the PC's DLSS/FSR support once known (nullptr while checking); monitor: the desktop size for the render scale labels.
    SettingsMenu(LiveSettings& live, std::string language, std::function<const FeatureReport*()> features, uint32_t monitor_width, uint32_t monitor_height, RendererBackend backend=RendererBackend::vulkan);
    void setup();   // renderer set_menu setup: fonts and style into the menu's ImGui context
    void build();   // renderer set_menu build: one frame of widgets; runs inside the renderer, so picks wait for commit()
    void commit();  // main loop, outside rendering: apply and save what build() picked
    bool capturing() const { return editor_.capturing().has_value(); }
    void capture_event(const SDL_Event&);   // key/mouse events while a binding is being captured
    void focus_lost() { editor_.focus(false); pad_editor_.focus(false); }
    // The menu was closed (Esc and Insert reach the host, not the menu): a capture still waiting for a pad press is dropped, and the Gamepad window closes with the menu.
    void menu_closed() { focus_lost(); pad_panel_.modal_open=false; }
    // The Controllers tab's gamepad window waits for a pad button while a binding is captured: GamepadInput's capture sink calls this for every pad press and release.
    // (Not part of capturing(): the menu keeps its mouse and keyboard while it waits, and Esc or a click cancels.)
    void pad_capture(PhysicalInput input,bool down) { pad_editor_.event(input,down); }
    void set_pad_status(std::function<PadStatus()> status) { pad_status_=std::move(status); }   // the connected gamepad, asked while the tab is drawn
    void upscaler_failed(UpscalerKind kind) { failed_.insert(kind); }   // could not start in this process: greyed out, changed in the launcher
    void frame_generation_failed(UpscalerKind kind) { failed_generation_.insert(kind); }
    // This run's DLSS 5 state, asked while the tab is drawn: running now, and why the renderer keeps it off (empty: nothing wrong). The feature report only says what the GPU could do.
    struct Dlss5Report { bool active=false; std::string reason; };
    void set_dlss5_status(std::function<Dlss5Report()> status) { dlss5_report_=std::move(status); }
    void set_open_debug(std::function<void()> open) { open_debug_=std::move(open); }   // the DLSS 5 tab's Open Debug button
    void set_mod_menu(mods::ModMenu* menu) { mod_menu_=menu; }   // the Mods tab and one tab per mod after the built-in tabs; nullptr: none
private:
    const char* tr(std::string_view key) const;
    void main_tab(float unit);
    void controls_tab(float unit);
    void reticle_tab(float unit);
    void controllers_tab(float unit);
    void effects_tab(float unit);
    void dlss5_tab(float unit);
    const Options& current() const { return pending_ ? *pending_ : live_.options(); }
    LiveSettings& live_;
    std::optional<Options> pending_;   // picked by build(), applied by commit()
    std::string language_;
    std::function<const FeatureReport*()> features_;
    uint32_t monitor_width_, monitor_height_;
    BackendUi backend_ui_;   // what the chosen graphics API can offer; the rest is hidden
    BindingEditor editor_;
    BindingEditor pad_editor_;   // the gamepad table (the Controllers tab)
    Bindings pad_seen_;          // the saved gamepad table as the tab last saw it: a different one is a change from outside
    GamepadPanelState pad_panel_;
    float heading_size_=32;   // the menu's title font size, as of the last frame
    std::function<PadStatus()> pad_status_;
    ui::Fonts fonts_;
    std::set<UpscalerKind> failed_, failed_generation_;
    std::function<Dlss5Report()> dlss5_report_;
    std::function<void()> open_debug_;
    mods::ModMenu* mod_menu_=nullptr;
};
}
