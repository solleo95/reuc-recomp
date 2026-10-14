#pragma once
#include "input_bindings.h"
#include <initializer_list>
#include <optional>
#include <vector>
namespace uc {
enum class PromptGesture:uint8_t {directional_motion,shake};
struct PromptChoice {std::vector<Binding> bindings;bool held=false;};
struct PromptAlternative {std::vector<PromptChoice> choices;std::optional<PromptGesture> gesture;};
struct PromptDescription {Action action;std::vector<PromptAlternative> alternatives;};
PromptDescription describe_prompt(const Bindings&,Action);
// Inline text glyphs: first assignment of each action, first input of each choice.
PromptDescription primary_prompt(const Bindings&,std::initializer_list<Action>);
}
