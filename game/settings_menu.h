#pragma once
#include "live_settings.h"
#include "feature_report.h"
#include "backend_ui.h"
#include "binding_editor.h"
#include "gamepad_panel.h"
#include "wiimote_panel.h"
#include "ui_fonts.h"
#include "menu_bridge.h"
#include "settings_registry.h"
#include <functional>
#include <mutex>
#include <optional>
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
    bool capturing() const { return editor_.capturing().has_value() || p2_editor_.capturing().has_value(); }
    void capture_event(const SDL_Event&);   // key/mouse events while a binding is being captured
    void focus_lost() { editor_.focus(false); pad_editor_.focus(false); wii_editor_.focus(false); p2_editor_.focus(false); p2_pad_editor_.focus(false); }
    // The menu was closed (Esc and Insert reach the host, not the menu): a capture still waiting for a pad press is dropped, and the Gamepad and Wii Remote windows close with the menu.
    void menu_closed() { focus_lost(); pad_panel_.modal_open=false; wii_panel_.modal_open=false; p2_pad_panel_.modal_open=false; }
    // The same for the Wii Remote window: WiimoteInput's capture sink calls this for every remote button press and release.
    void wii_capture(PhysicalInput input,bool down) { wii_editor_.event(input,down); }
    void set_wii_status(std::function<WiimoteStatus()> status) { wii_status_=std::move(status); }   // the connected Wii Remote, asked while the tab is drawn
    void set_wii_sync(std::function<void()> sync) { wii_sync_=std::move(sync); }   // request Bluetooth discovery/pairing from the in-game menu
    // The Controllers tab's gamepad window waits for a pad button while a binding is captured: GamepadInput's capture sink calls this for every pad press and release.
    // (Not part of capturing(): the menu keeps its mouse and keyboard while it waits, and Esc or a click cancels.)
    void pad_capture(PhysicalInput input,bool down) { (p2_pad_editor_.capturing() ? p2_pad_editor_ : pad_editor_).event(input,down); }   // whichever gamepad window waits for a press
    void set_pad_status(std::function<PadStatus()> status) { pad_status_=std::move(status); }   // the connected gamepad, asked while the tab is drawn
    // Local two-player play (this menu only; not offered where it is off: `available`). `device`: 1 a gamepad, 2 a Wii Remote, 3 a mouse or light gun (the second player's);
    // `asking`: the same numbers for a new device a question is open about (the modal window); `replaces`: a second player is already there; `problem` is what the devices layer
    // says is wrong (no access to /dev/input). The host loop gives a fresh snapshot every pass (the menu is drawn on the renderer's thread) and takes the player's answer through
    // the command function, called from commit().
    // `question_only`: the question opened the menu itself, so only its window is drawn, not the menu behind it.
    struct SecondPlayer { bool available=false,joined=false; int device=0; int asking=0; bool replaces=false; std::string problem; bool question_only=false; };
    enum class SecondCommand { disconnect,first,second,not_now };
    void set_second_player(const SecondPlayer& state) { std::lock_guard lock(second_mutex_); second_state_=state; }
    void set_second_command(std::function<void(SecondCommand)> command) { second_command_=std::move(command); }
    void upscaler_failed(UpscalerKind kind) { failed_.insert(kind); }   // could not start in this process: greyed out, changed in the launcher
    void frame_generation_failed(UpscalerKind kind) { failed_generation_.insert(kind); }
    // This run's DLSS 5 state, asked while the tab is drawn: running now, and why the renderer keeps it off (empty: nothing wrong). The feature report only says what the GPU could do.
    struct Dlss5Report { bool active=false; std::string reason; };
    void set_dlss5_status(std::function<Dlss5Report()> status) { dlss5_report_=std::move(status); }
    void set_open_debug(std::function<void()> open) { open_debug_=std::move(open); }   // the DLSS 5 tab's Open Debug button
    void set_mod_menu(mods::ModMenu* menu) { mod_menu_=menu; }   // the Mods tab and one tab per mod after the built-in tabs; nullptr: none
private:
    const char* tr(std::string_view key) const;
    bool shown(const char* id) const { return settings::shown(id,settings::desktop_menu); }   // the settings registry decides what this menu lists
    void main_tab(float unit);
    void controls_tab(float unit);
    void reticle_tab(float unit);
    void controllers_tab(float unit);
    void effects_tab(float unit);
    void dlss5_tab(float unit);
    void new_device_modal(float unit);
    // A tab's content as a page of its own below the title and the tab bar, which stay where they are: the page scrolls as a whole (a scrollbar when it does not fit). Room is
    // left under it for the error line.
    template<class Content> void page(const char* id,Content&& content);   // defined in settings_menu.cpp, used only there
    const Options& current() const { return pending_ ? *pending_ : live_.options(); }
    LiveSettings& live_;
    std::optional<Options> pending_;   // picked by build(), applied by commit()
    std::string language_;
    std::function<const FeatureReport*()> features_;
    uint32_t monitor_width_, monitor_height_;
    BackendUi backend_ui_;   // what the chosen graphics API can offer; the rest is hidden
    BindingEditor editor_;
    std::string binding_error_;   // the refusal the dialog shows (the editor's own error ends with the capture)
    BindingEditor pad_editor_;   // the gamepad table (the Controllers tab)
    Bindings pad_seen_;          // the saved gamepad table as the tab last saw it: a different one is a change from outside
    GamepadPanelState pad_panel_;
    BindingEditor wii_editor_;   // the Wii Remote table (the Controllers tab)
    Bindings wii_seen_;          // the saved Wii Remote table as the tab last saw it
    WiimotePanelState wii_panel_;
    std::function<WiimoteStatus()> wii_status_;
    std::function<void()> wii_sync_;
    BindingEditor p2_editor_;       // the second player's keyboard-and-mouse table (the Controls tab, "Player 2"): only its mouse entries act
    BindingEditor p2_pad_editor_;   // the second player's gamepad table (the Controllers tab)
    Bindings p2_seen_,p2_pad_seen_;   // the saved tables as the tabs last saw them: a different one is a change from outside
    GamepadPanelState p2_pad_panel_;
    int player_view_=0;             // the Controls tab shows this player's table
    std::mutex second_mutex_;
    SecondPlayer second_state_;                 // the host's snapshot (any thread, under the mutex)
    SecondPlayer second_view_;                  // what this frame draws (the renderer's thread)
    std::optional<SecondCommand> second_pick_;  // picked by build(), run by commit()
    std::function<void(SecondCommand)> second_command_;
    float heading_size_=32;   // the menu's title font size, as of the last frame
    std::function<PadStatus()> pad_status_;
    ui::Fonts fonts_;
    std::set<UpscalerKind> failed_, failed_generation_;
    std::function<Dlss5Report()> dlss5_report_;
    std::function<void()> open_debug_;
    mods::ModMenu* mod_menu_=nullptr;
};
}
