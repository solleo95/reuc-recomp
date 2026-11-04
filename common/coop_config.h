#pragma once
#include "gamepad_config.h"
#include <optional>
#include <string>
namespace uc {
// The "player2" object of config.json: what the second player's devices do. The keyboard-and-mouse table is the first player's kind of table but only its mouse entries
// ever act (there is no second keyboard); the gamepad object has the shape of the first player's "gamepad" object. The second player's Wii Remote has no table here:
// it uses the first player's "wiimote" bindings.
struct CoopSettings {
    Bindings bindings=default_bindings();
    GamepadSettings gamepad;
    bool operator==(const CoopSettings&) const=default;
};
std::optional<std::string> validate_coop(const CoopSettings&);
// No "player2" object gives the defaults; a malformed one throws std::invalid_argument("config: player2: ..."). Unknown keys are refused.
CoopSettings coop_from_config(const ConfigDocument&);
void store_coop(ConfigDocument&,const CoopSettings&);   // throws std::invalid_argument and leaves the document alone when the settings are invalid
}
