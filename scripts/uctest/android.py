"""Runs the game of a uctest scenario on an Android handheld (the AYN Thor) over adb while the harness stays on this computer.

The app (scripts/android/build.sh installs it) starts with `am start ... --es UC_AUTOSTART 1 --es UC_ARGS "<arguments>"`, and its test server listens on
two abstract sockets (game/test_pipe_posix.cpp) that `adb forward` maps to local TCP ports. The run folder (saves, config, captures) is
/sdcard/UmbrellaChronicles/autotest/<name>; run.log is the game's logcat, streamed here while it runs; the artifacts come back with `adb pull` and the run
folder and this run's session logs are removed from the device afterwards (the player's own logs stay). Enable with
`python -m uctest run <scenario> --android` (sets UC_ANDROID=1).

UC_ANDROID_DRIVER: the Vulkan driver package in /sdcard/UmbrellaChronicles/drivers to run with (e.g. a Turnip .zip); unset = the system's driver.
UC_ANDROID_SERIAL: the device, when adb sees more than one.
"""
import os
import shlex
import socket
import subprocess
import time
from pathlib import Path

PACKAGE = "org.reuc_recomp.umbrella_chronicles"
DEVICE_DIR = "/sdcard/UmbrellaChronicles"
ADB = ["adb"] + (["-s", os.environ["UC_ANDROID_SERIAL"]] if os.environ.get("UC_ANDROID_SERIAL") else [])
# Diagnostics of a run that reach the game as environment variables (GameActivity sets every UC_* extra), as the Deck runs pass them.
FORWARDED_ENV = ("UC_CPU_SAMPLE", "UC_FRAME_LOG", "UC_UBER_ONLY", "UC_PIPELINE_THREADS", "UC_PIPELINE_TRACE", "UC_SAVE_TRACE", "UC_TEST_REAL_PAD", "UC_POLL_BY_EVENT")


def enabled():
    return bool(os.environ.get("UC_ANDROID"))


def _free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def _shell(command, **kwargs):
    return subprocess.run(ADB + ["shell", command], **kwargs)


def _game_pid():
    result = _shell(f"pidof {PACKAGE}", capture_output=True, text=True)
    return result.stdout.split()[0] if result.returncode == 0 and result.stdout.split() else None


class AndroidRun:
    def __init__(self, name, run_dir):
        self.name = name
        self.run_dir = Path(run_dir)
        self.remote_dir = f"{DEVICE_DIR}/autotest/{name}"
        self.cmd_port, self.evt_port = _free_port(), _free_port()
        self.process = self.logcat = self.pid = None
        self.forwards = []
        _shell(f"rm -rf {self.remote_dir} && mkdir -p {self.remote_dir}", check=True)

    def send_folder(self, local_folder, remote_name):
        """Copies a local folder to <run folder>/<remote_name> (the saves)."""
        subprocess.run(ADB + ["push", str(local_folder), f"{self.remote_dir}/{remote_name}"], check=True, stdout=subprocess.DEVNULL)

    def send_file(self, local_file, remote_name):
        subprocess.run(ADB + ["push", str(local_file), f"{self.remote_dir}/{remote_name}"], check=True, stdout=subprocess.DEVNULL)

    def copy_player_config(self):
        """The run's config.json: a copy of the player's settings on the device (the run may change its copy, never theirs)."""
        _shell(f"[ -f {DEVICE_DIR}/config.json ] && cp {DEVICE_DIR}/config.json {self.remote_dir}/config.json; true", check=True)

    def start(self, args, env=None):
        """args: the game's arguments, device paths absolute. Returns a process that ends when the game's process does."""
        env = dict(env or {})
        for key, value in os.environ.items():
            if key.startswith(FORWARDED_ENV):
                env[key] = value
        for arg in args:
            if not arg or any(c.isspace() for c in arg):
                raise ValueError(f"the app splits UC_ARGS on whitespace: argument {arg!r} cannot pass")
        # sync: /sdcard is FUSE with a write-back cache, and the app reads through its own view of it: without the flush the game once read the
        # run's config.json cut at 8192 bytes (the pages written so far) and stopped at startup.
        _shell(f"sync; am force-stop {PACKAGE}", check=True)
        extras = {"UC_AUTOSTART": "1", "UC_DRIVER": os.environ.get("UC_ANDROID_DRIVER", ""), "UC_ARGS": " ".join(args), **env}
        command = f"am start -W -n {PACKAGE}/.LauncherActivity" + "".join(f" --es {shlex.quote(key)} {shlex.quote(value)}" for key, value in extras.items())
        _shell(command, check=True, stdout=subprocess.DEVNULL)
        deadline = time.monotonic() + 20
        while not (pid := _game_pid()):
            if time.monotonic() > deadline:
                raise TimeoutError("the app did not start on the device")
            time.sleep(0.2)
        self.pid = pid
        with open(self.run_dir / "run.log", "wb") as log:   # what the game writes to stderr, as on the other platforms (tag uc)
            self.logcat = subprocess.Popen(ADB + ["logcat", "-v", "raw", f"--pid={pid}", "-s", "uc"], stdout=log, stderr=subprocess.DEVNULL)
        self.process = subprocess.Popen(ADB + ["shell", f"while [ \"$(pidof {PACKAGE})\" = {pid} ]; do sleep 0.5; done"],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for port, suffix in ((self.cmd_port, "cmd"), (self.evt_port, "evt")):
            subprocess.run(ADB + ["forward", f"tcp:{port}", f"localabstract:uc-test-{self.name}-{suffix}"], check=True, stdout=subprocess.DEVNULL)
            self.forwards.append(port)
        return self.process

    def wait_listening(self, seconds=180.0):
        """Waits until the game's test sockets exist (a first run with a new driver compiles its pipelines before that)."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise TimeoutError("the game on the device exited before it listened")
            names = _shell("cat /proc/net/unix", capture_output=True, text=True).stdout
            if f"@uc-test-{self.name}-cmd" in names and f"@uc-test-{self.name}-evt" in names:
                return
            time.sleep(0.5)
        raise TimeoutError("the test sockets did not appear on the device")

    def _session_logs(self):
        if not self.pid:
            return []
        listing = _shell(f"ls {DEVICE_DIR}/logs", capture_output=True, text=True).stdout.split()
        return [name for name in listing if name.startswith("game-") and name.rsplit("-", 2)[-2:-1] == [self.pid]]

    def fetch(self, local_dir):
        """Pulls the run folder (all but the saves) and this run's session logs into local_dir."""
        local_dir = Path(local_dir)
        _shell("sync")   # what the game wrote (captures, logs) reaches the files adb reads
        for name in _shell(f"ls {self.remote_dir}", capture_output=True, text=True).stdout.split():
            if name != "saves":
                subprocess.run(ADB + ["pull", f"{self.remote_dir}/{name}", str(local_dir)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        sessions = self._session_logs()
        if sessions:
            (local_dir / "logs").mkdir(exist_ok=True)
            for name in sessions:
                subprocess.run(ADB + ["pull", f"{DEVICE_DIR}/logs/{name}", str(local_dir / "logs")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def close(self):
        """Stops the game, drops the forwards and removes the run folder and this run's session logs from the device."""
        for port in self.forwards:
            subprocess.run(ADB + ["forward", "--remove", f"tcp:{port}"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        sessions = self._session_logs()
        _shell(f"am force-stop {PACKAGE}")
        _shell(" ; ".join([f"rm -rf {self.remote_dir}"] + [f"rm -rf {DEVICE_DIR}/logs/{name}" for name in sessions] + [f"rmdir {DEVICE_DIR}/autotest 2>/dev/null; true"]))
        for process in (self.process, self.logcat):
            if process and process.poll() is None:
                process.terminate()
                try:
                    process.wait(5)
                except subprocess.TimeoutExpired:
                    process.kill()
