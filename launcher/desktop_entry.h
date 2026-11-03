#pragma once
#include <string_view>
namespace uc::launcher {
// Linux: the launcher of the Windows build (launcher/main.cpp) as a function. ui is the view it was started for, "desktop" or "deck" (see common/ui_mode.h); it is handed
// to the game when Play starts it.
int run_desktop_launcher(int argc, char** argv, std::string_view ui);
}
