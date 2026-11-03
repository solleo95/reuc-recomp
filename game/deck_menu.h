#pragma once
#include "binding_editor.h"
#include "deck_menu_nav.h"
#include "feature_report.h"
#include "gamepad_config.h"
#include "mods_page.h"
#include "live_settings.h"
#include "ui_fonts.h"
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
union SDL_Event;
namespace uc {
// The Steam Deck's in-game menu (the View button): every settings-registry item with the Deck bit, on pages, driven by the D-pad, the sticks, the standard buttons and the
// touchscreen. The game is paused while it is open (uc_main). Every pick goes through LiveSettings, which applies and saves it, like the desktop menu.
class DeckMenu {
public:
    DeckMenu(LiveSettings& live,std::string language,std::function<const FeatureReport*()> features,uint32_t monitor_width,uint32_t monitor_height);
    void setup();    // renderer set_menu setup: fonts into the menu's ImGui context
    void build();    // renderer set_menu build: one frame; runs inside the renderer, so picks wait for commit()
    void commit();   // main loop, outside rendering: apply and save what build() picked, close the menu when asked
    bool event(const SDL_Event&);   // pad, stick and key events while the menu is open; true: the menu used it
    void opened();                  // the menu was opened: focus goes back to the first row
    void set_close(std::function<void()> close) { close_=std::move(close); }
    void set_quit(std::function<void()> quit) { quit_=std::move(quit); }   // the Quit game page: A twice ends the game
    // The Mods page: the launcher's Mods tab (launcher/deck/mods_page) over these mods. `registry_changed` is called after a switch, the order or native code changed.
    // Without mods there is no such page.
    void set_mods(deck::ModsAccess access,std::function<void()> registry_changed);
    // The Buttons page waits for a pad button while a binding is captured: GamepadInput's capture sink calls this for every pad press and release.
    // The same pad event then reaches event(): when a capture was waiting it is the capture's (B bound to an action must not also be "back"), even though it has just ended the capture.
    // The same sink serves the gyro's hold button ("while a button is held"): the row waits for the next press of a trigger, a shoulder or a stick button.
    void pad_capture(PhysicalInput input,bool down) {
        if(hold_capturing_) {
            capture_took_=true;
            if(down && gyro_hold_allowed(input)) { std::lock_guard lock(mutex_); hold_result_=input; hold_capturing_=false; }   // another button is ignored: the row keeps waiting
            return;
        }
        if(editor_.capturing()) capture_took_=true;
        editor_.event(input,down);
    }
    void menu_closed();   // the menu was closed: a capture still waiting for a button is dropped
private:
    std::vector<settings::Page> pages() const;   // the Deck pages that exist in this run (no Mods page without a mod platform)
    const char* tr(std::string_view key) const;
    const Options& current() const { return pending_ ? *pending_ : live_.options(); }
    settings::Env env() const;
    void press(NavKey);                       // a key from a pad, the keyboard or a tap
    void hold(NavKey,bool down);              // a direction kept down repeats
    bool custom_press(std::string_view id,uint64_t now_ns);   // A or a tap on a custom row of its own; false: not one of them
    LiveSettings& live_;
    std::string language_;
    std::function<const FeatureReport*()> features_;
    uint32_t monitor_width_, monitor_height_;
    ui::Fonts fonts_;
    DeckNav nav_;
    std::optional<Options> pending_;          // picked by build(), applied by commit()
    std::function<void()> close_, quit_;
    int button_sub_=0;                        // the Buttons page's focused row: 0 the row itself, 1 its plus, 2 its cross
    bool quit_armed_=false, quit_requested_=false;   // the first press on Quit game asks, the second ends the game (any other key disarms)
    std::mutex mutex_;                        // the queue and the held key: event() runs on the main loop, build() inside the renderer
    std::deque<NavKey> queue_;
    bool close_requested_=false, moved_by_key_=false, moved_list_=false, list_was_open_=false;
    bool has_hold_=false; NavKey hold_key_=NavKey::down; uint64_t hold_next_ns_=0;
    float stick_scroll_=0;                    // right stick, -1..1: scrolls the rows
    int stick_dir_[2]={0,0};                  // the left stick as a D-pad: x and y, -1 / 0 / 1
    float press_x_=0, press_y_=0;             // where the finger went down on a row or an entry: a drag scrolls, a tap picks
    bool dragged_=false;
    BindingEditor editor_{default_pad_bindings(),BindingTable::gamepad};   // the Buttons page: the gamepad table; opened() reloads it from the saved options
    deck::RowCtx mod_rows(deck::FocusList&,float unit);   // the launcher's row context over this menu's fonts, texts and touch state
    std::optional<deck::ModsAccess> mods_access_;
    std::function<void()> registry_changed_;
    deck::ModsUi mods_ui_;
    deck::FocusList mods_focus_, settings_focus_, modal_focus_;   // the Mods page, a mod's settings window (under its list and text entry), the other windows
    deck::TouchDrag touch_;
    int mods_sub_=0;                          // the part of the focused mod row the D-pad has selected
    std::atomic<bool> typing_{false};         // a text setting is being typed: the keyboard goes to the text field
    uint64_t capture_since_ns_=0;             // a capture nobody answers is dropped after a few seconds
    bool capture_took_=false;                 // the pad event being dispatched went to a waiting capture
    bool hold_capturing_=false;               // the gyro's hold-button row waits for a pad press (capture_since_ns_ times it out)
    std::optional<PhysicalInput> hold_result_;   // the press that answered it, for build() to store
    // The right trackpad pressed past the soft threshold is the left mouse button of ImGui's cursor (the trackpad moves it; its click is not a mouse click here).
    bool right_pad_pressed_=false, click_open_=false;
    std::deque<bool> clicks_;                 // button transitions for build() to hand to ImGui
};
}
