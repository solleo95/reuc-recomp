"""Unequal render workloads must keep network simulation near realtime and in sync."""
import argparse
import contextlib
import json
import socket
import time

from .client import Game, ROOT
from .netplay import coop_menu, simulation_seconds, telemetry_rates
from .netplay_history import hashes
from .transport import PipeTransport


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", default="dist/uc.exe")
    args = parser.parse_args()
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    folder = ROOT / "build" / "autotest" / f"net-slow-peer-{time.time_ns()}"
    print(folder, flush=True)
    with contextlib.ExitStack() as stack:
        host = Game(exe=args.exe, run_dir=folder / "host", seconds=240, god=True,
            game_args=["--net-host", "--net-port", str(port), "--net-loopback", "--render-scale", "1"])
        client = Game(exe=args.exe, run_dir=folder / "client", seconds=240, god=True,
            game_args=["--net-join", "127.0.0.1", "--net-port", str(port), "--net-loopback", "--render-scale", "1"])
        for game in (host, client):
            game.transport_factory = lambda game=game: PipeTransport(game.name, 90, alive=lambda: game.process.poll() is None)
            stack.enter_context(game)
        coop_menu(host, client)
        client.set_options(render_scale=6, upscaler="off")
        changed = time.monotonic()
        host.wait(lambda _: time.monotonic()-changed >= 5, seconds=15, awaited="heavy client settled")
        initial = [dict(game.frame) for game in (host, client)]
        started = time.monotonic()
        last_input = started
        leads = []

        def sample(_):
            nonlocal last_input
            now = time.monotonic()
            if now-last_input >= .5:
                client.aim(.3 if int((now-started)*2) % 2 else -.3, .2, wait=False)
                last_input = now
            leads.append(abs(simulation_seconds(host.frame)-simulation_seconds(client.frame)))
            return now-started >= 20

        host.wait(sample, seconds=30, awaited="unequal workloads remain synchronized")
        elapsed = time.monotonic()-started
        rates = [telemetry_rates(begin, dict(game.frame), elapsed)
                 for begin, game in zip(initial, (host, client))]
        captures = [game.capture() for game in (host, client)]
        left, right = hashes(host), hashes(client)
        common = left.keys() & right.keys()
        report = {"rates": rates, "max_lead_seconds": max(leads), "captures": captures,
                  "matching_hashes": len(common), "last_confirmed": max(common, default=0)}
        (folder / "report.json").write_text(json.dumps(report, indent=2), encoding="utf8")
        print(json.dumps(report), flush=True)
        assert common and all(left[n] == right[n] for n in common), "confirmed states disagree"
        assert all(game.process.poll() is None for game in (host, client)), "peer exited"
        assert all(.9 <= rate["simulation_ratio"] <= 1.1 for rate in rates), "slow peer lost realtime simulation"
        assert max(leads) < .4, "host ran far ahead of client"
        print("Unequal-workload network check passed", flush=True)


if __name__ == "__main__":
    main()
