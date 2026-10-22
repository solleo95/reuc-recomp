"""Record enemy behaviour of one scene at 30 Hz and at a high update rate for scripts/ab_compare.py.

python ab_behaviour.py --scene train --chapter 1 --lead 60 --seconds 40 --hz 30 480 [--from after-ch1]
python ab_behaviour.py --scene episode --lead 0 --seconds 40 --hz 30 480

The harness rules apply: 1280x720 window, copied saves, muted, god mode, no system input. The scene is reached
deterministically at x8 (same lead in game seconds for every rate), then recorded at speed 1 with fps limit = hz while
the autoplay bot keeps playing. Each run writes build/autotest/ab-<scene><chapter>-<hz>-<time>/states.jsonl.
"""
import argparse
import json
import os
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from uctest import autoplay, scenarios, steps, vision  # noqa: E402
from uctest.client import Game, resolve_saves  # noqa: E402


USER_GRAPHICS = {"upscaler": "dlss", "upscale_mode": "balanced", "frame_generation": True, "frame_generation_multiplier": 2, "ray_tracing": True,
                 "rt_quality": "high", "motion_blur": "ultra", "bloom": True, "hdr": False}   # the owner's config.json graphics (hdr off: the harness window is SDR)


def start_episode(g):
    """Episode start used by the earlier monkey runs (build/episode_probe.py): checkpoint dialog, chapter 0."""
    steps.skip_movies(g)
    steps.select(g, "title_start")
    for _ in range(30):
        g.press("A", 2, x=-0.425, y=-0.05)
        g.advance(30)
        if g.frame.get("menu", [0, 0, 0, 0])[3] != 0:
            break
    else:
        g.fail("checkpoint dialog did not appear")
    for _ in range(30):
        g.press("A", 2, x=-0.35, y=0.2)
        g.advance(30)
        if g.frame.get("ui", [-1] * 6)[5] != 3:
            break
    else:
        g.fail("checkpoint dialog did not close")
    steps.wait_gameplay(g)


def record(args, hz):
    run = ROOT / "build" / "autotest" / f"ab-{args.scene}{args.chapter}-{hz}-{time.time_ns()}"
    saves = resolve_saves(args.from_) if args.from_ else None
    if args.trap:   # write-trap the guest words (e.g. an actor's position) from --trap-from game seconds into the recording; report: <run>/trap
        os.environ["UC_WRITE_TRAP"] = f"{run / 'trap'}|{args.trap}"
        os.environ["UC_WRITE_TRAP_ALL"] = "1"
    run.mkdir(parents=True, exist_ok=True)
    with Game(exe=args.exe, run_dir=run, speed=8, god=True, skip_tutorials=True, saves=saves, seconds=1800,
              game_args=[*([] if args.live else ["--deterministic"]), "--render-scale", "1", "--fps-limit", "120"]) as g:
        if args.scene == "episode":
            start_episode(g)
        else:
            scenarios.reach_gameplay(g, args.chapter)
        bot = autoplay.Autoplay(g, run / "autoplay.jsonl", speed=8, god=True, aim_only=() if args.second_wave else args.aim_only)
        bot.aim_min_id = args.min_id
        lead_start = g.frame["retraces"]
        lead_end = lead_start + args.lead * 60
        first_wave, armed, last_alive = set(), not args.second_wave, 0
        while g.frame["retraces"] < lead_end:
            if bot.step():
                break
            alive = {e["id"] for e in g.frame.get("enemies", []) if e["cls"] in args.aim_only and e.get("hp", 1) > 0 and e["id"] >= args.min_id}
            if not armed:   # wave 1 is shot normally; once it is dead the bot only aims at the class
                first_wave |= alive
                if alive:
                    last_alive = g.frame["retraces"]
                if first_wave and not alive and g.frame["retraces"] - last_alive >= args.quiet * 60:   # the wave spawns in bursts: wait for a quiet gap
                    armed = True
                    bot.aim_only = frozenset(args.aim_only)
                    vision.HEIGHT_OVERRIDE.update({c: args.aim_height for c in args.aim_only})
                    armed_at = g.frame["retraces"]
                    print("wave 1 dead at", (g.frame["retraces"] - lead_start) / 60, "s", flush=True)
                continue
            if args.second_wave:
                if g.frame["retraces"] >= armed_at + args.after_dead * 60:
                    break   # the next wave is on screen about now
            elif g.frame["retraces"] >= lead_start + args.min_lead * 60 and alive:
                break   # the aim-only enemies are on screen: slow down now, not at a fixed time
        g.set_speed(1)
        g.set_options(fps_limit=hz, **(USER_GRAPHICS if args.user_graphics else {}))
        g.wait(lambda f: f.get("speed") == 1, seconds=30)
        start = g.frame["retraces"]
        next_sample = next_shot = start
        with (run / "states.jsonl").open("w") as out:
            while g.frame["retraces"] < start + args.seconds * 60:
                reason = bot.step()
                if args.trap and g.frame["retraces"] >= start + args.trap_from * 60 and not (run / "trap.go").exists():
                    (run / "trap.go").write_text("x")
                if args.shots_every and g.frame["retraces"] >= next_shot:
                    next_shot = g.frame["retraces"] + args.shots_every * 60
                    g.capture()
                if g.frame["retraces"] >= next_sample:
                    out.write(json.dumps(g.frame) + "\n")
                    next_sample = g.frame["retraces"] + args.sample_ticks
                if reason:
                    out.write(json.dumps({"stop": reason}) + "\n")
                    break
        print(hz, run, flush=True)
    return run


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--scene", choices=["train", "episode"], default="train")
    p.add_argument("--chapter", type=int, default=1)
    p.add_argument("--from", dest="from_")
    p.add_argument("--lead", type=int, default=60, help="game seconds played at x8 before recording")
    p.add_argument("--seconds", type=int, default=40)
    p.add_argument("--hz", type=int, nargs="+", default=[30, 480])
    p.add_argument("--aim-only", type=lambda v: int(v, 0), nargs="*", default=[], help="actor classes the bot aims at but never fires at (e.g. 0x803FE3A0 hunters)")
    p.add_argument("--min-id", type=int, default=0, help="ignore --aim-only enemies below this actor id when counting waves (zombies share the hunters' class)")
    p.add_argument("--after-dead", type=float, default=4.0, help="with --second-wave: game seconds between the death of wave 1 and the slowdown")
    p.add_argument("--quiet", type=float, default=3.0, help="with --second-wave: game seconds without a live wave-1 enemy that count as the wave being dead")
    p.add_argument("--live", action="store_true", help="real clock instead of the deterministic virtual one (frame times jitter as in a player run)")
    p.add_argument("--user-graphics", action="store_true", help="apply the owner's upscaler, frame generation, ray tracing and motion blur for the recording")
    p.add_argument("--trap", default="", help="comma separated hex guest addresses to write-trap (UC_WRITE_TRAP)")
    p.add_argument("--trap-from", type=float, default=0.0, help="game seconds into the recording at which the trap is armed")
    p.add_argument("--second-wave", action="store_true", help="shoot the first --aim-only enemies normally; slow down and only aim at the next ones")
    p.add_argument("--sample-ticks", type=int, default=3, help="record a state every N retraces (1/60 s); 0 = every game frame")
    p.add_argument("--min-lead", type=int, default=0, help="game seconds before the first sighting of an --aim-only class ends the lead")
    p.add_argument("--aim-height", type=float, default=0.0, help="aim point height above the root of the --aim-only classes")
    p.add_argument("--shots-every", type=float, default=0, help="capture a screenshot every N game seconds while recording")
    p.add_argument("--exe", default=os.environ.get("UC_PROBE_EXE", "dist/uc.exe"))
    args = p.parse_args()
    if not args.second_wave:
        vision.HEIGHT_OVERRIDE.update({c: args.aim_height for c in args.aim_only})
    for hz in args.hz:
        record(args, hz)


if __name__ == "__main__":
    main()
