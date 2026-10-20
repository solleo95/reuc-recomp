#pragma once
#include "dlss5_settings.h"
#include <functional>
#include <string>
#include <string_view>

namespace uc::ui {
// The rows of the DLSS 5 tab, drawn from the settings table (dlss5_settings.h) for both the launcher and the in-game menu: per row the control (a segmented bar, a
// selector or a slider), a reset-to-default button and the label to its right; the Scaling and Advanced sections collapse; Debug has one button.
struct Dlss5PanelHooks {
    std::function<const char*(std::string_view)> tr;      // launcher translations
    std::function<void(const std::string&)> track;        // called after every widget with its id (the launcher's UI smoke finds them by it)
    std::function<void()> open_debug;                     // the Open Debug button
};
// Returns true when a setting changed. `ui` says what is usable (Hook Method bar / the other rows).
bool dlss5_panel(Dlss5Settings& settings, const Dlss5Ui& ui, const Dlss5PanelHooks& hooks);
}
