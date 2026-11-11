#pragma once
#include "input_probe.h"
#include <memory>
#include <span>
namespace uc {
constexpr uint32_t kNativeWeaponStep=0x08000000; // Port-only logical request bit, never a physical controller input.
InputContext game_input_context(const InputProbeSnapshot&,unsigned player=0);
InputSample game_input_buttons(InputContext,const ActionFrame&);
InputSample game_input_sample(const ppc::Ctx&,const InputSample&);
void game_input_init(NativeInput*,bool trace=false,NativeInput* second=nullptr);
// Auto QTE: the game completes every QTE by itself (button prompts and shakes). In network play every PC must run it with the same value (the host's), set before the first step.
void game_input_set_auto_qte(bool);
bool game_input_auto_qte();
void game_input_note_qte_prompt(std::span<const Action>);   // the prompt hook: the QTE actions of a prompt drawn now
void game_input_shutdown();
struct GameInputSnapshot;
std::shared_ptr<const GameInputSnapshot> game_input_checkpoint();
void game_input_restore(const GameInputSnapshot&);
}
void hle_GameActionMask(ppc::Ctx&);
void hle_GameWeaponUpdate(ppc::Ctx&);
void hle_GameActionOwnerUpdate(ppc::Ctx&);
void hle_GameMotionCount(ppc::Ctx&);
