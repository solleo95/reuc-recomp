// The Steam Deck launcher's tabs against the settings registry: which registry pages each tab shows and what the Deck launcher lists (host test).
#include "check.h"
#include "deck/settings_pages.h"
#include "gamepad_config.h"
#include <algorithm>
using namespace uc;
using namespace uc::settings;
using uc::deck::Tab;
static bool has(const std::vector<Page>& pages, Page page) { return std::find(pages.begin(), pages.end(), page) != pages.end(); }
int main() {
    const auto game = deck::tab_pages(Tab::game), graphics = deck::tab_pages(Tab::graphics), controls = deck::tab_pages(Tab::controls);
    CHECK(game == std::vector<Page>{Page::game});
    CHECK(graphics == (std::vector<Page>{Page::graphics, Page::effects}));
    CHECK(controls == (std::vector<Page>{Page::gamepad, Page::buttons}));   // no desktop Controls page, and no Reticle (the in-game menu only)
    // every registry page the Deck launcher lists belongs to exactly one tab (nothing is hidden by accident), and the Mods page is not a tab
    for (const auto page : pages_for(deck_launcher)) {
        const int tabs = has(game, page) + has(graphics, page) + has(controls, page);
        CHECK(tabs == 1);
    }
    // what the Deck launcher must not offer: upscalers, the HDR sliders, ray tracing, the desktop key and Wii Remote panels, window mode and renderer choice
    for (const char* id : {"renderer", "window_mode", "upscaling", "upscale_mode", "sharpening", "sharpness", "frame_generation", "hud_position", "hdr_peak", "hdr_paper_white",
                           "ray_tracing", "rt_quality", "dlss5", "ambient_occlusion", "pointer_device", "mouse_sensitivity", "prompt_style", "key_bindings",
                           "pad_panel", "wiimote", "reticle_preview", "mods_manager",
                           "crosshair_scale", "hide_tail", "hide_dot", "hide_marks", "hide_arc", "tail_length", "dot_size", "partner_opacity", "reticle_color_p1", "reticle_color_p2"})
        CHECK(!shown(id, deck_launcher));
    for (const char* id : {"crosshair_scale", "reticle_color_p1"}) CHECK(shown(id, deck_menu));   // the reticle stays in the in-game menu
    // what it offers: the gyro and sticks, the gamepad bindings, the reticle, the game toggles, scale/limit, effects
    for (const char* id : {"skip_intro", "skip_tutorials", "logs", "render_scale", "fps_limit", "anisotropy", "per_pixel_lighting", "motion_blur", "contact_shadows",
                           "bloom", "bloom_strength", "hdr", "hdr_ui", "gyro_mode", "gyro_sens_x", "gyro_sens_y", "aim_speed", "trackpad_sensitivity", "pointer_smoothing", "reload_zone", "pad_bindings"})
        CHECK(shown(id, deck_launcher));
    CHECK(shown("trackpad_sensitivity", deck_menu) && !shown("trackpad_sensitivity", desktop_menu) && !shown("trackpad_sensitivity", desktop_launcher));   // the Deck's trackpad only
    {   // the trackpad's slider opens the "Touch aim" group, right before the aim smoothing; it is the same setting as the mouse sensitivity (config mouse_sensitivity)
        const auto items = items_for(deck_launcher, Page::gamepad);
        size_t trackpad_at = 99, smoothing_at = 99;
        for (size_t i = 0; i < items.size(); ++i) { if (std::string_view(items[i]->id) == "trackpad_sensitivity") trackpad_at = i; if (std::string_view(items[i]->id) == "pointer_smoothing") smoothing_at = i; }
        CHECK(trackpad_at != 99 && smoothing_at == trackpad_at + 1);
        Options set; const Item& item = *items[trackpad_at];
        CHECK(item.get_int(set) == 100 && item.min == 10 && item.max == 300);   // 100% as the aim has always followed the cursor
        item.set_int(set, 150); CHECK(set.mouse_sensitivity == 150);
    }
    // the gyro's sensitivity and inversion mean nothing while the gyro is off: they are hidden then (the settings registry is shared with the in-game menu)
    Options options; Env env;
    options.gamepad.gyro.mode = GyroMode::off;
    for (const char* id : {"gyro_sens_x", "gyro_sens_y", "gyro_invert_x", "gyro_invert_y"}) CHECK(!shown(id, deck_launcher, options, env));
    for (const char* id : {"gyro_mode", "aim_speed", "aim_dead_zone", "camera_dead_zone"}) CHECK(shown(id, deck_launcher, options, env));   // the sticks do not depend on it
    options.gamepad.gyro.mode = GyroMode::always;
    for (const char* id : {"gyro_sens_x", "gyro_sens_y", "gyro_invert_x", "gyro_invert_y"}) CHECK(shown(id, deck_launcher, options, env));
    options.gamepad.gyro.mode = GyroMode::hold;
    for (const char* id : {"gyro_sens_x", "gyro_invert_y"}) CHECK(shown(id, deck_menu, options, env));
    // "while a button is held": the button to hold is chosen by pressing it, in the launcher and in the in-game menu alike; with another mode there is no such row
    for (const auto surface : {deck_launcher, deck_menu}) {
        options.gamepad.gyro.mode = GyroMode::hold; CHECK(shown("gyro_hold_button", surface, options, env));
        options.gamepad.gyro.mode = GyroMode::always; CHECK(!shown("gyro_hold_button", surface, options, env));
        options.gamepad.gyro.mode = GyroMode::off; CHECK(!shown("gyro_hold_button", surface, options, env));
    }
    CHECK(!shown("gyro_hold_button", desktop_menu) && !shown("gyro_hold_button", desktop_launcher));   // the desktop windows have their own combo for it
    {   // the row follows the mode row in the Controls list
        const auto items = items_for(deck_launcher, Page::gamepad);
        size_t mode_at = 99, hold_at = 99;
        for (size_t i = 0; i < items.size(); ++i) { if (std::string_view(items[i]->id) == "gyro_mode") mode_at = i; if (std::string_view(items[i]->id) == "gyro_hold_button") hold_at = i; }
        CHECK(mode_at != 99 && hold_at == mode_at + 1);
    }
    // what can be chosen as the hold button: any gamepad button (face, D-pad, View, sticks, shoulders, paddles) or a trigger; a stick direction, a key or the mouse cannot
    for (const PhysicalInput input : {PhysicalInput{PhysicalKind::pad_axis, 9}, PhysicalInput{PhysicalKind::pad_axis, 11}, PhysicalInput{PhysicalKind::pad_button, 9},
                                      PhysicalInput{PhysicalKind::pad_button, 10}, PhysicalInput{PhysicalKind::pad_button, 7}, PhysicalInput{PhysicalKind::pad_button, 8},
                                      PhysicalInput{PhysicalKind::pad_button, 0}, PhysicalInput{PhysicalKind::pad_button, 1}, PhysicalInput{PhysicalKind::pad_button, 11},
                                      PhysicalInput{PhysicalKind::pad_button, 16}, PhysicalInput{PhysicalKind::pad_button, 31}})
        CHECK(gyro_hold_allowed(input));
    for (const PhysicalInput input : {PhysicalInput{PhysicalKind::pad_axis, 0}, PhysicalInput{PhysicalKind::pad_axis, 1}, PhysicalInput{PhysicalKind::key, 9}, PhysicalInput{PhysicalKind::mouse, 1}})
        CHECK(!gyro_hold_allowed(input));
    {   // the recenter row: after the gyro button, only while the gyro is on, on the Deck surfaces only
        CHECK(shown("gyro_recenter", deck_menu) && shown("gyro_recenter", deck_launcher) && !shown("gyro_recenter", desktop_menu) && !shown("gyro_recenter", desktop_launcher));
        Options on;
        on.gamepad.gyro.mode = GyroMode::off; CHECK(!shown("gyro_recenter", deck_launcher, on, env));
        on.gamepad.gyro.mode = GyroMode::always; CHECK(shown("gyro_recenter", deck_launcher, on, env));
        on.gamepad.gyro.mode = GyroMode::hold; CHECK(shown("gyro_recenter", deck_launcher, on, env));
        const auto items = items_for(deck_launcher, Page::gamepad);
        size_t hold_at = 99, recenter_at = 99;
        for (size_t i = 0; i < items.size(); ++i) { if (std::string_view(items[i]->id) == "gyro_hold_button") hold_at = i; if (std::string_view(items[i]->id) == "gyro_recenter") recenter_at = i; }
        CHECK(hold_at != 99 && recenter_at == hold_at + 1);   // right under the gyro button
    }
    CHECK(!validate_gamepad(options.gamepad));   // the default hold button (the left trigger) is one of them
    std::puts("deck pages: tabs and registry passed");
}
