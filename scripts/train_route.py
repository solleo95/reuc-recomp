"""Run the learned train route through process-local input; never send OS input."""
import argparse
import pathlib
import re
import shutil
import subprocess
import time


def publish_command(temporary, control):
    deadline = time.monotonic() + 2
    while True:
        try:
            temporary.replace(control)
            return
        except PermissionError as error:
            if getattr(error, "winerror", None) not in (5, 32) or time.monotonic() >= deadline:
                raise
            time.sleep(0.01)  # Windows readers briefly deny replacement of their open handle.


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=pathlib.Path)
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parent.parent
    exe = (args.exe or root / "dist" / "uc.exe").resolve()
    folder = root / "build" / "autotest" / str(time.time_ns())
    folder.mkdir(parents=True)
    shutil.copytree(pathlib.Path(r"S:\Games\Umbrella Chronicles\saves"), folder / "saves")
    control = folder / "control.txt"
    log_path = folder / "run.log"
    sequence = 0
    with log_path.open("w", encoding="utf-8") as log:
        process = subprocess.Popen([str(exe), "--skip-intro", "--profile", "--seconds", "150",
                                    "--test-control", str(control), "--save-root", str(folder / "saves")],
                                   cwd=root, stdout=log, stderr=log)
        print(f"PID {process.pid}; log {log_path}", flush=True)

        def read_log():
            return log_path.read_text(encoding="utf-8", errors="replace")

        def command(x, y, held=0, capture=False):
            nonlocal sequence
            sequence += 1
            temporary = folder / "control-next.txt"
            temporary.write_text(f"{sequence} {x} {y} {held} {int(capture)}\n", encoding="ascii")
            publish_command(temporary, control)
            deadline = time.monotonic() + 10
            while f"[test-control] consumed {sequence}\n" not in read_log():
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError(f"No input acknowledgement; inspect {log_path}")
                time.sleep(0.05)

        def tap(x=0, y=0, wait=1.7):
            command(x, y)
            time.sleep(0.35)  # Menu hover trails a newly positioned aim; settle before confirming.
            command(x, y, 2048)
            time.sleep(0.15)
            command(x, y)
            time.sleep(wait)
            command(x, y, capture=True)

        # shortcut: fixed RBUE08 menu coordinates/timings; update after menu/save layout changes.
        time.sleep(3)
        tap()  # Opening movie.
        tap(wait=4)  # Save menu is visible before it starts accepting selection.
        tap(-0.425, -0.05, wait=2)  # Save slot one.
        tap(-0.5125, -0.0333)  # Train scenario.
        tap(-0.325, -0.2)  # Chapter one.
        tap(-0.24, -0.067)  # Rebecca.
        tap(0.625, 0.4)  # Confirm currently equipped SMG.
        tap(-0.35, 0.2, wait=2.5)  # Start confirmation.
        tap(wait=3)  # Mission movie.
        for _ in range(16):
            tap(wait=3)  # Tutorial text still has a typewriter/confirmation gate.
        measurement_begin = len(read_log())
        time.sleep(15)
        command(0, 0, capture=True)
        time.sleep(1)
        report = read_log()
        if "[mesh-material]" not in report:
            raise RuntimeError(f"Train geometry not observed; route not accepted: {log_path}")
        rates = [int(value) for value in re.findall(r"\[perf\] presents (\d+)", report[measurement_begin:])]
        screenshots = re.findall(r"debug screenshot: ([^\r\n]+)", report)
        if not rates or not screenshots:
            raise RuntimeError(f"Measurement or capture missing: {log_path}")
        print(f"Presentations/sec: {sum(rates)/len(rates):.2f}, min {min(rates)}, max {max(rates)}", flush=True)
        print(f"Capture: {screenshots[-1]}", flush=True)
        print("Visually verify train/HUD in the capture; mesh activity alone is not scene acceptance.", flush=True)
        print("Window remains visible until its 150-second diagnostic timeout.", flush=True)


if __name__ == "__main__":
    main()
