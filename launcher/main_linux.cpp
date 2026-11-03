// The Linux launcher: one program with both views. The Steam Deck's Game Mode one (deck/main_deck.cpp) and the one of the Windows build (main.cpp) differ in looks and
// in what they need from the machine; which one starts is decided here by `--ui desktop|deck`, then UC_UI, then the session (common/ui_mode.h).
#include "desktop_entry.h"
#include "deck/run_deck.h"
#include "ui_mode.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::optional<std::string_view> flag;
    std::vector<char*> arguments{argv[0]};   // what the view itself parses: everything but --ui
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ui") == 0) {
            if (i + 1 >= argc) { std::fprintf(stderr, "uc_launcher: missing value for --ui\n"); return 2; }
            flag = argv[++i];
        } else arguments.push_back(argv[i]);
    }
    const auto mode = uc::resolve_ui_mode(flag, std::getenv("UC_UI"), std::getenv("GAMESCOPE_WAYLAND_DISPLAY"));
    if (const auto* error = std::get_if<std::string>(&mode)) { std::fprintf(stderr, "uc_launcher: %s\n", error->c_str()); return 2; }
    const auto view = std::get<uc::UiMode>(mode);
    const std::string name(uc::ui_mode_name(view));
    std::fprintf(stderr, "[ui] launcher view %s\n", name.c_str());
    ::setenv("UC_UI", name.c_str(), 1);   // the game started from this launcher, and the launcher restarted after an update, keep the view (the game also gets --ui)
    arguments.push_back(nullptr);
    const int count = int(arguments.size()) - 1;
    return view == uc::UiMode::deck ? run_deck_launcher(count, arguments.data()) : uc::launcher::run_desktop_launcher(count, arguments.data(), name);
}
