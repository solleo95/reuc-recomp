"""Two-player co-op from chapter N of the train scenario: one bot per player plays until a game exits or the time runs out (hunts host crashes in later chapters).

python -m uctest.netplay_chapter --from after-ch2 --chapter 2 --speed 8 --max-minutes 8
A desync leaves desync-<frame>.ram (guest RAM of that frame and the one before) in both peers' folders; UC_NET_HASH_DETAIL=1 logs the RAM chunk hashes per hash frame.
"""
import argparse
import contextlib
import json
import re
import socket
import time

from .autoplay import Autoplay
from .client import ROOT, Game, StepFailed, resolve_saves
from .netplay import coop_menu
from .netplay_history import hashes
from .transport import PipeTransport

ATTEMPTS = 6   # the chapter page sometimes takes the click for its first card (the press that opened it is still being read): then the menu is started again


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", default="dist/uc.exe")
    parser.add_argument("--from", dest="saves_from", default="after-ch1-clean", help="checkpoint name or saves directory with the chapter unlocked and no mid-chapter save point (that one asks two questions at the save slot)")
    parser.add_argument("--chapter", type=int, default=2, choices=range(1, 5))
    parser.add_argument("--speed", type=int, default=8, choices=range(1, 33), metavar="1..32")
    parser.add_argument("--max-minutes", type=float, default=8)
    parser.add_argument("--game-arg", action="append", default=[], help="extra uc.exe argument for both games")
    args = parser.parse_args()
    folder = ROOT / "build" / "autotest" / f"net-chapter{args.chapter}-{time.time_ns()}"
    print(folder, flush=True)
    saves = resolve_saves(args.saves_from)
    report = {"chapter": args.chapter, "speed": args.speed, "result": None}
    for attempt in range(1, ATTEMPTS + 1):
        run = folder / f"try{attempt}"
        port = free_port()
        with contextlib.ExitStack() as stack:
            common = ["--net-loopback", *args.game_arg]
            seconds = int(args.max_minutes * 60) + 120
            host = Game(exe=args.exe, run_dir=run / "host", speed=args.speed, seconds=seconds, god=True, skip_tutorials=True, saves=saves,
                        game_args=["--net-host", "--net-port", str(port), *common])
            client = Game(exe=args.exe, run_dir=run / "client", speed=args.speed, seconds=seconds, god=True, skip_tutorials=True, saves=saves,
                          game_args=["--net-join", "127.0.0.1", "--net-port", str(port), *common])
            for game in (host, client):
                game.transport_factory = lambda game=game: PipeTransport(game.name, 90, alive=lambda: game.process.poll() is None)
                stack.enter_context(game)
            coop_menu(host, client, f"chapter_{args.chapter}")
            host.advance(300)   # the log names the running script once a second (script=s00c2: scenario 0, chapter 2)
            started = re.findall(r"script=s\d+c(\d+)", (host.run_dir / "run.log").read_text(errors="replace"))
            if not started or int(started[-1]) != args.chapter:
                print(f"attempt {attempt}: the menu started chapter {started[-1:] or 'unknown'}, not {args.chapter}", flush=True)
                continue
            report["attempt"], report["run"] = attempt, str(run)
            bots = [Autoplay(game, run / f"autoplay-{name}.jsonl", god=True, speed=args.speed, sweep=args.chapter > 1) for game, name in ((host, "host"), (client, "client"))]
            deadline = time.monotonic() + args.max_minutes * 60
            result = "time_limit"
            try:
                while time.monotonic() < deadline:
                    for name, game, bot in (("host", host, bots[0]), ("client", client, bots[1])):
                        if game.process.poll() is not None:
                            result = f"{name}_exited"
                            raise StopIteration
                        reason = bot.step()
                        if reason:
                            result = f"{name}_{reason}"
                            raise StopIteration
            except StopIteration:
                pass
            except StepFailed as error:
                result = "step_failed"
                report["error"] = str(error)
                report["artifacts"] = error.artifacts
            time.sleep(1)
            report["result"] = result
            report["exit_codes"] = {"host": host.process.poll(), "client": client.process.poll()}
            report["frames"] = {"host": host.frame.get("retraces"), "client": client.frame.get("retraces")}
            report["kills"] = [bot.kills for bot in bots]
            report["last_screen"] = {"host": host.frame.get("context"), "client": client.frame.get("context")}
            try:
                left, right = hashes(host), hashes(client)
                both = left.keys() & right.keys()
                report["matching_hashes"] = len(both)
                report["mismatched_hashes"] = [n for n in both if left[n] != right[n]]
            except AssertionError as error:   # the log says the history ran out or the states diverged
                report["netplay_error"] = str(error)
        break
    else:
        report["result"] = "wrong_chapter"
    (folder / "report.json").write_text(json.dumps(report, indent=2, default=str), encoding="utf8")
    print(json.dumps(report, default=str), flush=True)
    return 0 if report["result"] == "time_limit" else 1


if __name__ == "__main__":
    raise SystemExit(main())
