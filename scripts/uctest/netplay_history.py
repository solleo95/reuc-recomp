"""A live pair must wait for delayed input instead of exhausting rollback history."""
import contextlib
import re
import socket
import time

from .client import Game, ROOT
from .netem import DelayRelay


def hashes(game):
    log = (game.run_dir / "run.log").read_text(errors="replace")
    assert "Network outage exceeded" not in log, "rollback history exhausted"
    assert "Network desync" not in log, "confirmed states diverged"
    return {int(frame): value for frame, value in re.findall(r"\[netplay\] frame (\d+) hash ([0-9A-F]+)", log)}


def main():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    folder = ROOT / "build" / "autotest" / f"net-history-{time.time_ns()}"
    print(folder, flush=True)
    with contextlib.ExitStack() as stack:
        relay = DelayRelay(port)
        stack.callback(relay.close)
        host = stack.enter_context(Game(run_dir=folder / "host", seconds=120,
            game_args=["--net-host", "--net-port", str(port), "--net-loopback", "--render-scale", "1"]))
        client = stack.enter_context(Game(run_dir=folder / "client", seconds=120,
            game_args=["--net-join", "127.0.0.1", "--net-port", str(relay.port), "--net-loopback", "--render-scale", "1"]))
        host.wait(lambda _: min(max(hashes(host), default=0), max(hashes(client), default=0)) >= 240,
                  seconds=30, awaited="both peers confirmed initial frames")
        before = min(max(hashes(host)), max(hashes(client)))
        relay.set_profile(5)
        client.aim(.3, .2, wait=False)
        started = time.monotonic()
        host.wait(lambda _: time.monotonic() - started >= 6, seconds=12, awaited="delayed input survived")
        for game in (host, client):
            assert game.process.poll() is None, "peer exited during input delay"
            hashes(game)
        relay.set_profile(0)
        host.wait(lambda _: min(max(hashes(host)), max(hashes(client))) >= before + 480,
                  seconds=30, awaited="both peers resumed confirmation")
        host_hashes, client_hashes = hashes(host), hashes(client)
        common = host_hashes.keys() & client_hashes.keys()
        assert max(common) >= before + 480
        assert all(host_hashes[frame] == client_hashes[frame] for frame in common)
        host_log = (host.run_dir / "run.log").read_text(errors="replace")
        assert "waiting for peer at frame" in host_log, "history backpressure was not exercised"
        assert any(int(count) for count in re.findall(r"rollback count (\d+)", host_log)), "late input did not rewind"
        relay.check()
        print(f"History recovery passed; confirmed through {max(common)}. Artifacts: {folder}", flush=True)


if __name__ == "__main__":
    main()
