#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace uc {
// The view the launcher and the game start: the one of the Windows build (desktop) or the Steam Deck's Game Mode one (deck).
enum class UiMode { desktop, deck };
std::string_view ui_mode_name(UiMode mode);   // "desktop" | "deck"
// flag: the value of --ui; env_ui: UC_UI; gamescope: GAMESCOPE_WAYLAND_DISPLAY (nullptr = not set). The flag beats UC_UI, which beats the session (a non-empty
// gamescope display = Game Mode). A value that is neither "desktop" nor "deck" is an error text, never a silent choice.
std::variant<UiMode, std::string> resolve_ui_mode(std::optional<std::string_view> flag, const char* env_ui, const char* gamescope);
}
