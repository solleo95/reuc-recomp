#pragma once
#include "ppc_rt.h"
#include <optional>
namespace uc {
struct PacingMode { bool unlock_60=false; uint32_t game_hz=0; };
// The game update rate a frame rate limit chooses (Options::fps_limit): 30 or less the original 30 Hz, 31..60 the retrace-locked 60 Hz,
// above 60 or unlimited (0) a timer step at the limit (480 Hz ceiling); game_hz_override (--game-hz) wins.
PacingMode pacing_for_limit(std::optional<uint32_t> fps_limit, uint32_t game_hz_override, bool unlock_60fps);
void game_pacing_set_mode(PacingMode);   // any thread; the guest picks it up at its next frame wait
// Pause: the game clock steps by zero (its step and step_scale are 0), the game keeps looping and rendering. Any thread.
void game_pacing_set_paused(bool);
bool game_pacing_paused();
struct GamePacingStats { uint64_t calls,retraces,timestep_ns,threshold_two,adjusted,wait_ns; };   // wait_ns: time the guest idled in the frame wait, i.e. headroom
void game_pacing_init(bool unlock, uint32_t game_hz = 0);   // game_hz > 0: experimental timer-driven step instead of the retrace wait
GamePacingStats game_pacing_stats();
float game_step_scale();   // real step / nominal frame of the game clock (1 in the original mode)
void pacing_call_body(ppc::Ctx& c, uint32_t frame, uint32_t entry);   // run an AOT body after its lowered stwu of `frame` bytes
// Generated step fixes (game/pacing_fixes.inc, written by tools/step_audit). Every template is the identity when the
// step scale is 1 or 0 (pause) and when the caller's LR does not match the record.
enum class PacingTemplate : uint8_t { scale, scale_ret, pow_alpha };
struct PacingFix { uint32_t entry, lr; PacingTemplate tmpl; uint8_t reg; int8_t power; };
struct PacingFixTable { const PacingFix* begin_; const PacingFix* end_; const PacingFix* begin() const { return begin_; } const PacingFix* end() const { return end_; } };
PacingFixTable pacing_fix_table();
void pacing_apply_record(ppc::Ctx& c, const PacingFix& fix, uint32_t frame);   // runs body entry+4 with the record's adjustment
}
#define PACING_ENTRY(entry, frame) void hle_PacingFix_##entry(ppc::Ctx& c);
#define PACING_FIX(entry, lr, tmpl, reg, power)
#include "pacing_fixes.inc"
#undef PACING_ENTRY
#undef PACING_FIX
void hle_GameFrameWait(ppc::Ctx& c);
void hle_ActorStepTime(ppc::Ctx& c);
void hle_FixedPlaybackStep(ppc::Ctx& c);
void hle_HudNameDelay(ppc::Ctx& c);
void hle_PropStep(ppc::Ctx& c);
void hle_PhysicsStep(ppc::Ctx& c);
void hle_PhysicsHit(ppc::Ctx& c);
void hle_PhysicsAutoDisable(ppc::Ctx& c);
void hle_PhysicsBodyDestroy(ppc::Ctx& c);
void hle_FlightStep(ppc::Ctx& c);
void hle_FlightPathStep(ppc::Ctx& c);
void hle_WorldRaycast(ppc::Ctx& c);
void hle_VectorAddStep(ppc::Ctx& c);
void hle_JumpBlendStep(ppc::Ctx& c);
void hle_CrossfadeStep(ppc::Ctx& c);
void hle_FlightPhaseStep(ppc::Ctx& c);
void hle_CameraHistoryStep(ppc::Ctx& c);
void hle_DeferredActorStep(ppc::Ctx& c);
void hle_ModelHistoryIdentity(ppc::Ctx& c);
void hle_RumbleStep(ppc::Ctx& c);
void hle_TrailHistoryStep(ppc::Ctx& c);
void hle_UiPhysicsStep(ppc::Ctx& c);
void hle_TimelineStep(ppc::Ctx& c);
void hle_WeaponTrailStep(ppc::Ctx& c);
void hle_EffectRetireStep(ppc::Ctx& c);
void hle_TargetRefreshStep(ppc::Ctx& c);
void hle_SoundFrameStep(ppc::Ctx& c);
void hle_SoundRetryStep(ppc::Ctx& c);
void hle_SoundWaitStep(ppc::Ctx& c);
void hle_HomeMenuStep(ppc::Ctx& c);
