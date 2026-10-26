"""Check the waiting screen with a late peer, then cancellation without a peer."""
import contextlib
import socket
import time
from subprocess import Popen
from unittest.mock import patch

from .client import Game, ROOT


def wait_state(game, predicate, label):
    # Before guest startup there are no frame events; query the live UI thread.
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        state = game.state()
        if predicate(state):
            return state
        time.sleep(.05)
    game.fail(f"timeout waiting for {label}", capture=False)


def main():
    with socket.socket() as port_probe:
        port_probe.bind(("127.0.0.1", 0))
        port = port_probe.getsockname()[1]
    folder = ROOT / "build" / "autotest" / f"net-startup-{time.time_ns()}"
    args = ["--net-port", str(port), "--net-loopback"]
    with contextlib.ExitStack() as stack:
        host = stack.enter_context(Game(run_dir=folder / "host", seconds=120,
                                       game_args=["--net-host", *args]))
        wait_state(host, lambda frame: frame.get("network_waiting") is True, "visible peer waiting screen")
        time.sleep(2)
        assert host.state().get("network_waiting") is True
        client = stack.enter_context(Game(run_dir=folder / "client", seconds=120,
                                         game_args=["--net-join", "127.0.0.1", *args]))
        for game in (host, client):
            wait_state(game, lambda frame: frame.get("network_waiting") is False and frame.get("presents", 0) > 0,
                       "waiting screen closed and game presenting")
        print("Late peer: waiting screen shown, then dismissed", flush=True)
    for game in (host, client):
        text = game.log_path("runtime.log").read_text(encoding="utf-8", errors="replace")
        for marker in ("[system]", "[gpu]", "[settings]", "[netplay]", "[session] log closed normally"):
            assert marker in text, (marker, game.log_path("runtime.log"))
        assert game.log_path("frames.csv").read_text().startswith("t_ms,interval_ms,")
    with Game(run_dir=folder / "cancel", seconds=120, game_args=["--net-host", *args]) as host:
        wait_state(host, lambda frame: frame.get("network_waiting") is True, "waiting screen before cancellation")
        process = host.process
        host.finish()
    assert process.returncode == 0, process.returncode
    assert "waiting screen shown" in host.log_path("runtime.log").read_text(encoding="utf-8")
    print(f"Cancellation passed. Artifacts: {folder}", flush=True)


if __name__ == "__main__":
    def default_frame_logs(*args, **kwargs):
        # Exercise the player's default logs/ destination, rather than the harness CSV override.
        kwargs["env"] = {key: value for key, value in kwargs["env"].items() if key != "UC_FRAME_LOG"}
        return Popen(*args, **kwargs)
    with patch("uctest.client.subprocess.Popen", side_effect=default_frame_logs):
        main()
