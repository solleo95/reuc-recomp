"""Hand-written pacing hooks in game/game_pacing.cpp, shared by the timing scanners. Keep in sync with project/hle_map.txt."""
from pathlib import Path
import re

HOOKS = {
    "ActorStepTime": "pause only; animated-model elapsed time; not a general rate correction",
    "FixedPlaybackStep": "conditional: only LR 800C799C is scaled",
    "FlightStep": "conditional mode scaling; displacement helper only",
    "FlightPathStep": "conditional mode scaling of path-distance input only",
    "FlightPhaseStep": "conditional: phase speed in states 32/34; body entry 801C56F4",
    "VectorAddStep": "conditional: only LR 801C387C falling-bat displacement",
    "JumpBlendStep": "conditional: only LR 80200B8C monkey residual blend",
    "CrossfadeStep": "conditional: LR 8016975C layer crossfade elapsed time divided by step scale (playback already per update); body entry 80165614",
    "CameraHistoryStep": "history sampling uses nominal frames; body entry 801B0AEC",
    "DeferredActorStep": "only staggered action countdown +8488 uses nominal frames; body entry 801A5168",
    "RumbleStep": "pattern samples advance at nominal-frame cadence; body entry 80136EF0",
    "TrailHistoryStep": "history samples advance at nominal-frame cadence; camera projection runs every call; body entry 80162A6C",
    "UiPhysicsStep": "angular drag uses nominal-frame scale; gravity coefficient supplied by frame boundary; body entry 802447DC",
    "TimelineStep": "timeline clock advances in nominal frames before event comparisons; body entry 80211788",
    "WeaponTrailStep": "weapon trail samples advance in nominal frames; fade retains dt; body entry 8006DBF8",
    "EffectRetireStep": "effect retirement grace counter advances in nominal frames; update body runs every call; body entry 801B9170",
    "TargetRefreshStep": "target refresh countdown advances in nominal frames at caller 801297B0; body entry 8012AEEC",
    "SoundFrameStep": "sound frame callbacks (voice envelopes and stop delays) run at nominal-frame cadence; body entry 803AA7B4",
    "SoundRetryStep": "conditional: LR8004A6FC compensates the caller's retry decrement and consumes nominal frames; body entry8004C920",
    "SoundWaitStep": "conditional: LR 8017AC74/8017AF5C stream-wait countdown consumes nominal frames; body entry 80049004",
    "HomeMenuStep": "home menu timeouts and animations run at nominal-frame cadence with latched input edges; body entry 803C1BFC",
    "ModelHistoryIdentity": "conditional: LR 801929B0 scales history numerator f28; other matrix identity calls unchanged",
    "WorldRaycast": "conditional: only LR 80200DFC landing-ray reach",
    "PhysicsStep": "solver input scaled; body entry 801172A8",
    "PhysicsHit": "force converted to preserve impulse; body entry 80117468",
    "PhysicsAutoDisable": "quiet-step countdown normalized; seconds countdown retains solver dt",
    "PhysicsBodyDestroy": "releases fractional quiet-step state; body entry 800EA9B4",
    "PropStep": "spring body gated above 60 Hz; body entry 8021027C",
    "HudNameDelay": "only counter +7752 normalized; body entry 8014D36C",
    "GameFrameWait": "frame boundary: timestep, pause, nominal ticks and sole-reader coefficients",
}
BODY_HOOKS = {"800C7910": "ActorStepTime", "801C44BC": "FlightPathStep",
              "801C56F4": "FlightPhaseStep", "80123D18": "JumpBlendStep", "80165614": "CrossfadeStep",
              "801B0AEC": "CameraHistoryStep", "801A5168": "DeferredActorStep", "80136EF0": "RumbleStep",
              "80162A6C": "TrailHistoryStep", "802447DC": "UiPhysicsStep", "80211788": "TimelineStep",
              "8006DBF8": "WeaponTrailStep", "801B9170": "EffectRetireStep", "8012AEEC": "TargetRefreshStep",
              "803AA7B4": "SoundFrameStep", "8004C920": "SoundRetryStep", "80049004": "SoundWaitStep",
              "803C1BFC": "HomeMenuStep", "80044D18": "WorldRaycast", "801172A8": "PhysicsStep",
              "80117468": "PhysicsHit", "800EA9B4": "PhysicsBodyDestroy",
              "8021027C": "PropStep", "8014D36C": "HudNameDelay"}


def hle_map_hooks(path: Path, names) -> dict:
    """Address -> name for every `@ADDR Name` binding in hle_map.txt whose name is in `names`."""
    found = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        match = re.match(r"@([0-9A-Fa-f]{8})\s+(\w+)", line)
        if match and match[2] in names:
            found[match[1].upper()] = match[2]
    return found
