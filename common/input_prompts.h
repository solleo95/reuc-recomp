#pragma once
#include "input_bindings.h"
#include "gamepad_config.h"
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>
namespace uc {
enum class PromptStyle:uint8_t {monochrome,sculpted};
enum class PromptGesture:uint8_t {directional_motion,shake};
struct PromptChoice {std::vector<Binding> bindings;bool held=false;};
struct PromptAlternative {std::vector<PromptChoice> choices;std::optional<PromptGesture> gesture;};
// Which controller the prompts are for: the keyboard and mouse (default), a gamepad of one family, or the Wii Remote; gyro: the gestures (shake, swing) are done with the
// gamepad's motion (the Wii Remote always has its own).
struct PromptDevice {bool gamepad=false;PadFamily family=PadFamily::generic;bool gyro=false;bool wiimote=false;bool operator==(const PromptDevice&) const=default;};
struct PromptDescription {Action action;std::vector<PromptAlternative> alternatives;PromptDevice device;};
// `bindings` is the keyboard table for the keyboard device, the gamepad table for a gamepad device and the Wii Remote table for the remote; any other kind is refused (std::invalid_argument).
PromptDescription describe_prompt(const Bindings&,Action,const PromptDevice& device={});
// Inline text glyphs: first assignment of each action, first input of each choice.
PromptDescription primary_prompt(const Bindings&,std::initializer_list<Action>,const PromptDevice& device={});
// Two buttons pressed together (A+B prompt): first assignment of each, one alternative; no alternative while either action is cleared.
PromptDescription chord_prompt(const Bindings&,Action first,Action second,const PromptDevice& device={});
// What a gamepad input is called on a controller of this family ("A", "Cross", "RT", "R2", "ZR", "D-pad Up", "Left Stick Up"...) and a Wii Remote button as printed on it
// ("A", "B", "1", "+", "Home", "Z"; the family does not matter); empty for inputs that are neither.
std::string pad_glyph_label(PhysicalInput,PadFamily);
// The device the prompts show now. ControlType::automatic follows the last device used (pad_last_used) while a pad is connected. The family is the player's model
// choice or, for "automatic", the connected pad's. The gyro prompts need the gyro switched on in the settings and present on the pad.
PromptDevice resolve_prompt_device(ControlType,PadModel,bool pad_last_used,bool pad_connected,PadFamily connected_family,bool gyro_enabled,bool pad_has_gyro);
}
