"""Runs the game of a uctest scenario on the Steam Deck (`ssh deck`) while the harness stays on the PC.

The game starts as `ssh deck ... ./uc --test-server <name>`; the two Unix sockets of its test server (commands in, events out) are forwarded to local TCP
ports by one `ssh -L`. Saves and config go over as a tar, the artifacts (run.log, captures, logs) come back as a tar, and the run folder on the Deck is
removed afterwards: the Deck has little free space. Enable with `python -m uctest run <scenario> --deck` (sets UC_DECK=1).
"""
import os
import shlex
import socket
import subprocess
import time

HOST = os.environ.get("UC_DECK_HOST", "deck")
REMOTE = os.environ.get("UC_DECK_DIR", "~/uc-run")   # where scripts/linux/deck.py stage put uc, data/ and game/
RUNTIME = "/run/user/1000"
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
        environment = os.environ.get("UC_DECK_ENV", DISPLAY_ENV) + "".join(f" {key}={shlex.quote(value)}" for key, value in (env or {}).items())
        command = f"cd {REMOTE} && env {environment} ./uc {' '.join(shlex.quote(a) for a in args)} > {self.remote_dir}/run.log 2>&1"
        self.process = subprocess.Popen(SSH + [command], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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
