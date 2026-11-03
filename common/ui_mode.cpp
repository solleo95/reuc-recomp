#include "ui_mode.h"

namespace uc {
std::string_view ui_mode_name(UiMode mode) { return mode == UiMode::deck ? "deck" : "desktop"; }

namespace {
std::variant<UiMode, std::string> parse(std::string_view text, const char* where) {
    if (text == "desktop") return UiMode::desktop;
    if (text == "deck") return UiMode::deck;
    return std::string("invalid value for ") + where + ": \"" + std::string(text) + "\" (expected desktop or deck)";
}
}  // namespace

std::variant<UiMode, std::string> resolve_ui_mode(std::optional<std::string_view> flag, const char* env_ui, const char* gamescope) {
    if (flag) return parse(*flag, "--ui");
    if (env_ui) return parse(env_ui, "UC_UI");
    return gamescope && *gamescope ? UiMode::deck : UiMode::desktop;
}
}  // namespace uc
