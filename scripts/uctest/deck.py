"""Runs the game of a uctest scenario on the Steam Deck (`ssh deck`) while the harness stays on the PC.

The game starts as `ssh deck ... ./uc --test-server <name>`; the two Unix sockets of its test server (commands in, events out) are forwarded to local TCP
ports by one `ssh -L`. Saves and config go over as a tar, the artifacts (run.log, captures, logs) come back as a tar, and the run folder on the Deck is
removed afterwards: the Deck has little free space. Enable with `python -m uctest run <scenario> --deck` (sets UC_DECK=1).
"""
import os
import re
import shlex
import socket
import subprocess
import time
from pathlib import Path

HOST = os.environ.get("UC_DECK_HOST", "deck")
REMOTE = os.environ.get("UC_DECK_DIR", "~/uc-run")   # where scripts/linux/deck.py stage put uc, data/ and game/
RUNTIME = "/run/user/1000"
SHORTCUT_ID = os.environ.get("UC_DECK_SHORTCUT", "9709607294513184768")   # the owner's non-Steam shortcut of ~/uc-run/uc-deck.sh (Game Mode runs)
# The Desktop Mode session on native Wayland with the Deck's own SDL3 (3.4: Wayland, Steam Deck controller); override the whole string with UC_DECK_ENV.
# The SDL3 we build (3.2.0 against the Windows-hosted sysroot) has no Wayland backend, so scripts/linux/deck.py stage does not ship it.
DISPLAY_ENV = "XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 SDL_VIDEODRIVER=wayland"
SSH = ["ssh", "-o", "BatchMode=yes", "-o", "ServerAliveInterval=15", HOST]


def enabled():
    return bool(os.environ.get("UC_DECK"))


def _free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def _ssh(command, **kwargs):
    return subprocess.run(SSH + [command], **kwargs)


class DeckRun:
    def __init__(self, name):
        self.name = name
        self.remote_dir = f"{REMOTE}/autotest/{name}"
        self.cmd_port, self.evt_port = _free_port(), _free_port()
        self.forward = None
        self.process = None

    def send_folder(self, local_folder, remote_name):
        """tar a local folder into the run folder on the Deck (the saves)."""
        _ssh(f"mkdir -p {self.remote_dir}/{remote_name}", check=True)
        with subprocess.Popen(["tar", "-cf", "-", "-C", str(local_folder), "."], stdout=subprocess.PIPE) as tar:
            subprocess.run(SSH + [f"tar -xf - -C {self.remote_dir}/{remote_name}"], stdin=tar.stdout, check=True)

    def send_file(self, local_file, remote_name):
        with open(local_file, "rb") as source:
            subprocess.run(SSH + [f"cat > {self.remote_dir}/{remote_name}"], stdin=source, check=True)

    def start(self, args, env=None):
        """args: the game's arguments after ./uc, paths already relative to or inside the run folder. Returns the ssh process (it ends with the game)."""
        if os.environ.get("UC_DECK_GAMEMODE"):
            return self._start_in_game_mode(args, env)
        environment = os.environ.get("UC_DECK_ENV", DISPLAY_ENV) + "".join(f" {key}={shlex.quote(value)}" for key, value in (env or {}).items())
        command = f"cd {REMOTE} && env {environment} ./uc {' '.join(shlex.quote(a) for a in args)} > {self.remote_dir}/run.log 2>&1"
        self.process = subprocess.Popen(SSH + [command], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        sockets = [f"{RUNTIME}/uc-test-{self.name}-cmd.sock", f"{RUNTIME}/uc-test-{self.name}-evt.sock"]
        self.forward = subprocess.Popen(SSH[:-1] + ["-N", "-o", "ExitOnForwardFailure=yes", "-L", f"{self.cmd_port}:{sockets[0]}", "-L", f"{self.evt_port}:{sockets[1]}", HOST],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return self.process

    def _start_in_game_mode(self, args, env=None):
        """The run the owner can watch: Steam starts the non-Steam shortcut of uc-deck.sh in Game Mode (gamescope shows it like any game); next-run.sh carries this run's arguments.
        The returned ssh process ends when the selected executable has appeared, then exited."""
        env = dict(env or {})
        for key, value in os.environ.items():
            if key.startswith(("UC_CPU_SAMPLE", "UC_FRAME_LOG", "UC_EXE", "LLVM_PROFILE_FILE", "RADV_", "MESA_", "AMD_", "mesa_")):   # diagnostics of the run, and the driver's switches: the profiler (its output lands in the run folder, which comes back with the artifacts)
                env[key] = value
        lines = [f"export {key}={shlex.quote(value)}" for key, value in env.items()]
        if "UC_CPU_SAMPLE" in env:   # absolute: the game may change its working directory
            lines.append(f'export UC_CPU_SAMPLE_OUT="{self.remote_dir.replace("~", "$HOME", 1)}/cpu-samples.txt"')
        lines += ["set -- " + " ".join(shlex.quote(a) for a in args), f"exec > {self.remote_dir}/run.log 2>&1"]
        _ssh(f"mkdir -p {self.remote_dir} && cat > {REMOTE}/next-run.sh", input=("\n".join(lines) + "\n").encode(), check=True)
        # Linux comm is limited to 15 bytes; staged test executable names are ASCII.
        process = shlex.quote(re.escape(env.get("UC_EXE", "./uc").rsplit("/", 1)[-1][:15]))
        launch = (f"pkill -x uc; XDG_RUNTIME_DIR={RUNTIME} DISPLAY=:0 steam -ifrunning steam://rungameid/{SHORTCUT_ID} >/dev/null 2>&1; "
                  f"for i in $(seq 1 120); do pgrep -x {process} >/dev/null && break; sleep 0.5; done; while pgrep -x {process} >/dev/null; do sleep 1; done")
        self.process = subprocess.Popen(SSH + [launch], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if os.environ.get("UC_DECK_MONITOR"):   # what the game uses while it runs: scripts/linux/deck_monitor.py (it ends with the game)
            with open(Path(__file__).resolve().parents[1] / "linux" / "deck_monitor.py", "rb") as source:
                subprocess.run(SSH + [f"cat > {self.remote_dir}/deck_monitor.py"], stdin=source, check=True)
            # a systemd unit: a plain background process of an ssh session is killed when the session ends
            _ssh(f"systemctl --user stop uc-monitor 2>/dev/null; systemd-run --user --unit=uc-monitor --collect python3 {self.remote_dir}/deck_monitor.py {self.remote_dir}/monitor.csv", stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        sockets = [f"{RUNTIME}/uc-test-{self.name}-cmd.sock", f"{RUNTIME}/uc-test-{self.name}-evt.sock"]
        self.forward = subprocess.Popen(SSH[:-1] + ["-N", "-o", "ExitOnForwardFailure=yes", "-L", f"{self.cmd_port}:{sockets[0]}", "-L", f"{self.evt_port}:{sockets[1]}", HOST],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return self.process

    def wait_listening(self, seconds=120.0):
        """Waits until the game's test sockets exist on the Deck (it listens after its start-up and the shader warm-up is NOT part of that)."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise TimeoutError("the game on the Deck exited before it listened")
            if _ssh(f"test -S {RUNTIME}/uc-test-{self.name}-cmd.sock -a -S {RUNTIME}/uc-test-{self.name}-evt.sock").returncode == 0:
                return
            time.sleep(0.3)
        raise TimeoutError("the test sockets did not appear on the Deck")

    def fetch(self, local_dir):
        """tar the run folder back (everything but the saves) into local_dir."""
        with subprocess.Popen(SSH + [f"tar -cf - -C {self.remote_dir} --exclude=./saves ."], stdout=subprocess.PIPE) as remote:
            subprocess.run(["tar", "-xf", "-", "-C", str(local_dir)], stdin=remote.stdout, check=False)
        # the game's own session logs live next to uc, not in the run folder
        with subprocess.Popen(SSH + [f"tar -cf - -C {REMOTE} logs 2>/dev/null"], stdout=subprocess.PIPE) as remote:
            subprocess.run(["tar", "-xf", "-", "-C", str(local_dir)], stdin=remote.stdout, check=False)

    def close(self):
        """Stops what is left of the game, forgets the forward and removes the run folder and the session logs from the Deck."""
        if self.forward:
            self.forward.terminate()
        # pkill -x (exact process name): -f would match this very command line and kill the shell before the rm
        _ssh(f"pkill -x uc; rm -rf {self.remote_dir} {REMOTE}/logs {REMOTE}/debug; rmdir {REMOTE}/autotest 2>/dev/null; true")
        if self.process and self.process.poll() is None:
            self.process.terminate()
