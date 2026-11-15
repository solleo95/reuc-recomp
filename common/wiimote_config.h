#pragma once
#include "input_config.h"
#include <optional>
#include <string>
namespace uc {
// The "wiimote" object of config.json: whether the remote is read at all, pointer smoothing, the accelerometer threshold for waves/swings, the nunchuk stick's dead zone,
// the remote's own binding table, and whether an accelerometer shake reloads during normal gameplay.
struct WiimoteSettings {
    bool enabled=true;
    bool reload_with_shake=true;  // separate setting: a free gameplay shake reloads; never stored in the binding table
    float smoothing=.35f;          // 0..0.9: the share of the pointer's old place that stays at each new IR reading (the camera's dots shake)
    float swing_threshold=1.5f;    // g (0.5..4): a push of the hand in the screen plane stronger than this is a swing or a shake
    float stick_dead_zone=.10f;    // 0..0.5 of the nunchuk stick's travel
    Bindings bindings=default_wii_bindings();
    bool operator==(const WiimoteSettings&) const=default;
};
std::optional<std::string> validate_wiimote(const WiimoteSettings&);
// No "wiimote" object gives the defaults; a malformed or out-of-range one throws std::invalid_argument("config: wiimote: ..."). Unknown keys are refused.
WiimoteSettings wiimote_from_config(const ConfigDocument&);
void store_wiimote(ConfigDocument&,const WiimoteSettings&);   // throws std::invalid_argument and leaves the document alone when the settings are invalid
}
