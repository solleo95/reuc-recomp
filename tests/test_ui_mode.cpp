// resolve_ui_mode: --ui beats UC_UI beats the session (gamescope = Game Mode); anything unrecognised is an error, never a silent choice.
#include "check.h"
#include "ui_mode.h"
#include <string>

using uc::UiMode;
static bool is(const std::variant<UiMode, std::string>& r, UiMode mode) { return std::holds_alternative<UiMode>(r) && std::get<UiMode>(r) == mode; }
static bool failed(const std::variant<UiMode, std::string>& r) { return std::holds_alternative<std::string>(r); }

int main() {
    CHECK(is(uc::resolve_ui_mode({}, nullptr, nullptr), UiMode::desktop));
    CHECK(is(uc::resolve_ui_mode({}, nullptr, "gamescope-0"), UiMode::deck));
    CHECK(is(uc::resolve_ui_mode({}, nullptr, ""), UiMode::desktop));   // set but empty: not a gamescope session
    CHECK(is(uc::resolve_ui_mode(std::string_view("desktop"), nullptr, "gamescope-0"), UiMode::desktop));
    CHECK(is(uc::resolve_ui_mode(std::string_view("deck"), nullptr, nullptr), UiMode::deck));
    CHECK(is(uc::resolve_ui_mode({}, "deck", nullptr), UiMode::deck));
    CHECK(is(uc::resolve_ui_mode({}, "desktop", "gamescope-0"), UiMode::desktop));   // UC_UI beats the session
    CHECK(is(uc::resolve_ui_mode(std::string_view("desktop"), "deck", nullptr), UiMode::desktop));   // the flag beats UC_UI
    CHECK(failed(uc::resolve_ui_mode(std::string_view("Deck"), nullptr, nullptr)));
    CHECK(failed(uc::resolve_ui_mode(std::string_view(""), nullptr, nullptr)));
    CHECK(failed(uc::resolve_ui_mode({}, "x", nullptr)));
    CHECK(failed(uc::resolve_ui_mode({}, "", nullptr)));
    const auto bad = uc::resolve_ui_mode(std::string_view("sideways"), nullptr, nullptr);
    CHECK(failed(bad) && std::get<std::string>(bad).find("sideways") != std::string::npos);
    CHECK(uc::ui_mode_name(UiMode::desktop) == "desktop" && uc::ui_mode_name(UiMode::deck) == "deck");
    return 0;
}
