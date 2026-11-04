#pragma once
#include "pointer_devices.h"
#include <intrin.h>   // before SDL: SDL's endian header redefines a builtin otherwise
#include <SDL3/SDL.h>
#include <memory>
namespace uc {
// Windows only. Every mouse and light gun as a device of its own through Raw Input (WM_INPUT, read in SDL's message hook: the window procedure stays SDL's). The device id is
// the Raw Input device handle as SDL's mouse id has it, so a mouse SDL reports and the same mouse here are one device. Listening costs the system nothing: the registration
// is only "sink" (events also while the window is not in front). capture(device,true) takes the legacy mouse messages away for the whole process (RIDEV_NOLEGACY): from then
// on the cursor does not move and every mouse, the first player's too, reaches the game through poll(); capture(device,false) for the last captured device gives them back.
std::unique_ptr<PointerBackend> make_raw_input_backend(SDL_Window*);
}
