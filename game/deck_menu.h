#pragma once
#include "binding_editor.h"
#include "deck_menu_nav.h"
#include "feature_report.h"
#include "menu_bridge.h"
#include "live_settings.h"
#include "ui_fonts.h"
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
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
    void set_mod_menu(mods::ModMenu* menu) { mods_=menu; }   // the Mods page; without one there is no such page
    // The Buttons page waits for a pad button while a binding is captured: GamepadInput's capture sink calls this for every pad press and release.
    // The same pad event then reaches event(): when a capture was waiting it is the capture's (B bound to an action must not also be "back"), even though it has just ended the capture.
    void pad_capture(PhysicalInput input,bool down) { if(editor_.capturing()) capture_took_=true; editor_.event(input,down); }
    void menu_closed();   // the menu was closed: a capture still waiting for a button is dropped
private:
    std::vector<settings::Page> pages() const;   // the Deck pages that exist in this run (no Mods page without a mod platform)
    const char* tr(std::string_view key) const;
    const Options& current() const { return pending_ ? *pending_ : live_.options(); }
    settings::Env env() const;
    void press(NavKey);                       // a key from a pad, the keyboard or a tap
    void hold(NavKey,bool down);              // a direction kept down repeats
    LiveSettings& live_;
    std::string language_;
    std::function<const FeatureReport*()> features_;
    uint32_t monitor_width_, monitor_height_;
    ui::Fonts fonts_;
    DeckNav nav_;
    std::optional<Options> pending_;          // picked by build(), applied by commit()
    std::function<void()> close_;
    std::mutex mutex_;                        // the queue and the held key: event() runs on the main loop, build() inside the renderer
    std::deque<NavKey> queue_;
    bool close_requested_=false, moved_by_key_=false, moved_list_=false, list_was_open_=false;
    bool has_hold_=false; NavKey hold_key_=NavKey::down; uint64_t hold_next_ns_=0;
    float stick_scroll_=0;                    // right stick, -1..1: scrolls the rows
    int stick_dir_[2]={0,0};                  // the left stick as a D-pad: x and y, -1 / 0 / 1
    float press_x_=0, press_y_=0;             // where the finger went down on a row or an entry: a drag scrolls, a tap picks
    bool dragged_=false;
    BindingEditor editor_{default_pad_bindings(),BindingTable::gamepad};   // the Buttons page: the gamepad table; opened() reloads it from the saved options
    mods::ModMenu* mods_=nullptr;
    uint64_t capture_since_ns_=0;             // a capture nobody answers is dropped after a few seconds
    bool was_mods_page_=false;
    bool capture_took_=false;                 // the pad event being dispatched went to a waiting capture
    // The right trackpad pressed past the soft threshold is the left mouse button of ImGui's cursor (the trackpad moves it; its click is not a mouse click here).
    bool right_pad_pressed_=false, click_open_=false;
    std::deque<bool> clicks_;                 // button transitions for build() to hand to ImGui
};
}
