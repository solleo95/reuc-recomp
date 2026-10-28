#pragma once
#include "gamepad_math.h"
#include "input_config.h"
#include <optional>
#include <string>
namespace uc {
enum class ControlType:uint8_t { automatic,keyboard_mouse,gamepad };   // which device the on-screen prompts show; both devices always work
enum class PadModel:uint8_t { automatic,xbox,playstation,nintendo,generic };   // automatic: the family of the connected gamepad
enum class StickRole:uint8_t { none,left,right };
// The "gamepad" object of config.json: control type and model, what each stick does, curves, the gyro, and the gamepad's own binding table.
struct GamepadSettings {
    ControlType type=ControlType::automatic;
    PadModel model=PadModel::automatic;
    StickRole aim_stick=StickRole::left,camera_stick=StickRole::right;
    StickCurve aim;
    float camera_dead_zone=.10f;
    GyroSettings gyro;
    PhysicalInput gyro_hold{PhysicalKind::pad_axis,9};   // the left trigger: with GyroMode::hold the gyro aims only while it is held
    Bindings bindings=default_pad_bindings();
    bool operator==(const GamepadSettings&) const=default;
};
std::optional<std::string> validate_gamepad(const GamepadSettings&);   // ranges, distinct stick roles, a valid gamepad table
// No "gamepad" object gives the defaults; a malformed or out-of-range one throws std::invalid_argument("config: ..."). Unknown keys are refused.
GamepadSettings gamepad_from_config(const ConfigDocument&);
void store_gamepad(ConfigDocument&,const GamepadSettings&);   // throws std::invalid_argument and leaves the document alone when the settings are invalid
}
