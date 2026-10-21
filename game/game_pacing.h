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
}
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
