#pragma once
#include "input_probe.h"
namespace uc {
constexpr uint32_t kNativeWeaponStep=0x08000000; // Port-only logical request bit, never a physical controller input.
InputContext game_input_context(const InputProbeSnapshot&);
InputSample game_input_buttons(InputContext,const ActionFrame&);
InputSample game_input_sample(const ppc::Ctx&,const InputSample&);
void game_input_init(NativeInput*,bool trace=false);
void game_input_shutdown();
}
void hle_GameActionMask(ppc::Ctx&);
void hle_GameWeaponUpdate(ppc::Ctx&);
void hle_GameActionOwnerUpdate(ppc::Ctx&);
void hle_GameMotionCount(ppc::Ctx&);
