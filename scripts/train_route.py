"""Run the learned train route through process-local input; never send OS input.

Superseded by `python -m uctest run train_gameplay|controls|perf` (docs/testing/test-server.md); kept until the owner removes it."""
import argparse
import json
import pathlib
import re
import shutil
import subprocess
import sys
import time


def require_native_actions(report,cleared=False):
    if "fatal:" in report:
        raise RuntimeError("Native control route crashed")
    actions = re.findall(r"\[native-action\] (\w+) object [0-9A-F]+ accepted 1", report)
    if cleared:
        if "Reload" not in actions or any(name in actions for name in ("Grenade","Knife")):
            raise RuntimeError("Cleared controls fired an action or Reload was not accepted")
        return
    missing = [name for name in ("Reload", "Grenade", "Knife") if name not in actions]
    if missing or actions.count("Knife") < 2:
        raise RuntimeError(f"Native actions not accepted: {missing}; knife accepts {actions.count('Knife')}")


def tutorial_accepts_input(report):
    states=re.findall(r"\[native-tutorial\] ([0-9A-F]+)",report)
    return bool(states) and int(states[-1],16)==0x20000000


def latest_context(report):
    contexts = re.findall(r"\[native-context\] (\w+)", report)
    return contexts[-1] if contexts else "inactive"


def gameplay_rates(report, context="inactive"):
    rates=[]
    contaminated=False
    for line in report.splitlines():
        state=re.match(r"\[native-context\] (\w+)",line)
        if state:
            contaminated |= state[1]!=context
            context=state[1]
        if "debug screenshot:" in line:
            contaminated=True
        rate=re.match(r"\[perf\] presents (\d+)",line)
        if rate:
            if context=="gameplay" and not contaminated:
                rates.append(int(rate[1]))
            contaminated=False
    return rates


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
    parser.add_argument("--render-scale", type=float, default=1)
    parser.add_argument("--prompt-style",choices=("monochrome","sculpted"),default="monochrome")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--unlock-60fps", action="store_true")
    parser.add_argument("--input-probe", action="store_true")
    parser.add_argument("--audio-audit", action="store_true")
    parser.add_argument("--gx-gaps", action="store_true")
    parser.add_argument("--native-controls", action="store_true")
    parser.add_argument("--progression", action="store_true")
    parser.add_argument("--bindings",choices=("default","cleared","rebound"),default="default")
    args = parser.parse_args()
    if (args.progression or args.bindings!="default") and not args.native_controls:
        parser.error("progression/binding profiles require --native-controls")
    if not 0.5 <= args.render_scale <= 6:
        parser.error("720p render multiplier must be0.5..6")
    if not 1 <= args.width <= 3840 or not 1 <= args.height <= 2160:
        parser.error("output dimensions must be positive and at most 3840x2160")
    root = pathlib.Path(__file__).resolve().parent.parent
    exe = (args.exe or root / "dist" / "uc.exe").resolve()
    folder = root / "build" / "autotest" / str(time.time_ns())
    folder.mkdir(parents=True)
    import localenv   # imported here because tests load this file by path
    shutil.copytree(localenv.saves_dir() or sys.exit(localenv.NOT_SET), folder / "saves")
    control = folder / "control.txt"
    log_path = folder / "run.log"
    config_path=folder/"config.json"
    bindings={}
    if args.bindings=="cleared":
        bindings={"grenade":[],"knife":[]}
    elif args.bindings=="rebound":
        bindings={"fire":[{"kind":"key","code":9},{"kind":"key","code":12}],
                  "grenade":[{"kind":"key","code":11},{"kind":"interact_fire"}],
                  "knife":[{"kind":"key","code":5},{"kind":"interact_motion"}]}
    config_path.write_text(json.dumps({"input":{"version":1,"bindings":bindings},"graphics":{"prompt_style":args.prompt_style}}),encoding="utf-8")
    sequence = 0
    last_aim = (0.0,0.0)
    last_held = 0
    with log_path.open("w", encoding="utf-8") as log:
        lifetime="260" if args.progression else "80"
        process = subprocess.Popen([str(exe), "--skip-intro", "--profile", "--seconds", lifetime,
                                    "--render-scale", str(args.render_scale), "--width", str(args.width), "--height", str(args.height),
                                    "--test-control", str(control), "--save-root", str(folder / "saves"),"--config",str(config_path),"--fps-limit","0"]   # tests run unlocked
                                   + (["--audio-audit"] if args.audio_audit else [])
                                   + (["--gx-gaps"] if args.gx_gaps else [])
                                   + (["--unlock-60fps"] if args.unlock_60fps else [])
                                   + (["--input-probe"] if args.input_probe or args.native_controls else []),
                                   cwd=root, stdout=log, stderr=log)
        print(f"PID {process.pid}; log {log_path}", flush=True)

        def read_log():
            return log_path.read_text(encoding="utf-8", errors="replace")

        def publish(fields):
            nonlocal sequence
            sequence += 1
            temporary = folder / "control-next.txt"
            temporary.write_text(fields(sequence)+"\n", encoding="ascii")
            publish_command(temporary, control)
            deadline = time.monotonic() + 10
            while f"[test-control] consumed {sequence}\n" not in read_log():
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError(f"No input acknowledgement; inspect {log_path}")
                time.sleep(0.05)

        def event(kind,code=0,value=0,x=0,y=0,capture=False):
            publish(lambda sequence:f"v2 {sequence} {kind} {code} {value} {x} {y} {int(capture)}")

        def command(x, y, held=0, capture=False):
            nonlocal last_aim,last_held
            if not args.native_controls:
                publish(lambda sequence:f"{sequence} {x} {y} {held} {int(capture)}")
                return
            if (x,y)!=last_aim:
                event("motion",x=(x-last_aim[0])*args.width/2,y=(y-last_aim[1])*args.height/2)
                last_aim=(x,y)
            fire=(1024,"key",9) if args.bindings=="rebound" else (1024,"mouse",3)
            for mask,kind,code in [(2048,"mouse",1),fire,(4096,"key",41)]:
                if (last_held^held)&mask:
                    event(kind,code,int(bool(held&mask)))
            last_held=held
            if capture:event("motion",capture=True)

        def key_tap(code):
            event("key",code,1); time.sleep(.08); event("key",code,0)

        def gameplay():
            return latest_context(read_log())=="gameplay"

        captured_glyphs=[0]

        def capture_new_glyphs(report):
            # Capture the screen once per newly covered prompt glyph before the route dismisses it.
            glyphs=report.count("covered 1")
            if glyphs>captured_glyphs[0]:
                captured_glyphs[0]=glyphs; command(*last_aim,capture=True); time.sleep(.5)

        def ready_for_gameplay():
            deadline=time.monotonic()+25
            while not gameplay():
                if process.poll() is not None or time.monotonic()>=deadline:
                    raise RuntimeError(f"Gameplay input context unavailable: {log_path}")
                report=read_log()
                if tutorial_accepts_input(report):
                    capture_new_glyphs(report)
                    command(0,0,2048); time.sleep(.08); command(0,0)
                else:
                    time.sleep(.05)

        def tap(x=0, y=0, wait=1.7, capture=False):
            command(x, y)
            time.sleep(0.35)  # Menu hover trails a newly positioned aim; settle before confirming.
            capture_new_glyphs(read_log())
            command(x, y, 2048)
            time.sleep(0.15)
            command(x, y)
            time.sleep(wait)
            if capture:
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
        if args.native_controls:
            ready_for_gameplay()
            time.sleep(1); command(0,0,capture=True); time.sleep(.5)  # First gameplay frame: stable view for renderer comparisons.
            command(0,0,4096); time.sleep(.15); command(0,0)
            deadline=time.monotonic()+3
            while "pause-state 00000002" not in read_log():
                if process.poll() is not None or time.monotonic()>=deadline:
                    raise RuntimeError(f"Actual pause state not observed: {log_path}")
                time.sleep(.05)
            time.sleep(.6)  # Original pause panel opening animation; context comes from state, not time.
            command(0,0,capture=True)
            command(0,0,4096); time.sleep(.15); command(0,0); time.sleep(.5)
            ready_for_gameplay()
            if args.bindings=="rebound":
                event("key",9,1);event("key",12,1)
                segment_begin=len(read_log())
                event("key",9,0);time.sleep(.15)
                if any(int(mask,16)&1024 for mask in re.findall(r"\[input-probe\][^\r\n]*released ([0-9A-F]+)",read_log()[segment_begin:])):
                    raise RuntimeError("Releasing one of two Fire bindings released the game action")
                event("key",12,0)
                event("key",9,1);event("focus",value=0);event("key",9,0)
                event("focus",value=1);event("key",9,1);event("key",9,0)
            for _ in range(3):
                command(.25,-.3,1024); time.sleep(.15); command(.25,-.3); time.sleep(.25)
            time.sleep(.7)  # Release the original post-shot animation before requesting Reload.
            ready_for_gameplay(); key_tap(21)  # R: actual reload request.
            time.sleep(.6); ready_for_gameplay(); event("wheel",1,1); time.sleep(.3)
            ready_for_gameplay(); key_tap(22); time.sleep(.3)
            ready_for_gameplay(); key_tap(11 if args.bindings=="rebound" else 10); time.sleep(.8)
            knife_key=5 if args.bindings=="rebound" else 25
            ready_for_gameplay(); event("key",knife_key,1); time.sleep(1); event("key",knife_key,0)
            ready_for_gameplay(); command(.1,-.2,2048); time.sleep(.15)
            command(.4,-.2,2048); time.sleep(.15); command(.4,-.2)  # Directional knife gesture.
            ready_for_gameplay()
            command(.4,-.2,2048); time.sleep(.08)
            command(.4,-.2,3072); time.sleep(.08); command(.4,-.2)  # Native grenade chord.
            ready_for_gameplay()
            command(.4,-.2,capture=True)
            time.sleep(1)  # Exclude screenshot readback from the presentation interval.
            before_measurement=read_log()
            measurement_begin=len(before_measurement)
            command(.25,-.3,1024); time.sleep(5); command(.25,-.3)
            report=read_log()
            require_native_actions(report,cleared=args.bindings=="cleared")
            print("Native actions:",re.findall(r"\[native-action\] ([^\r\n]+)",report),flush=True)
            rates=gameplay_rates(report[measurement_begin:],latest_context(before_measurement))
            print("Context at measurement end:","gameplay" if gameplay() else "modal",flush=True)
            print("Presentation samples:",rates,flush=True)
            if args.progression:
                ready_for_gameplay()
                key_tap(26)  # Return from the tested SMG to the infinite-reserve pistol.
                deadline=time.monotonic()+180
                targets=[(-.35,-.05),(.35,-.05),(0,-.05),(-.15,.15),(.15,.15)]
                index=0
                guard_knife=False
                captured_at=time.monotonic()-10
                while time.monotonic()<deadline and process.poll() is None:
                    report=read_log()
                    context=latest_context(report)
                    if guard_knife and context!="gameplay":
                        event("key",knife_key,0);guard_knife=False
                    if tutorial_accepts_input(report):
                        capture_new_glyphs(report)
                        command(0,0,2048); time.sleep(.08); command(0,0)
                    elif context=="qte":
                        # Real timed reversals only; the original game owns the success threshold.
                        for x in (-.2,.2)*4:
                            command(x,0); time.sleep(.05)
                    elif context=="gameplay":
                        if args.bindings!="cleared" and not guard_knife:
                            event("key",knife_key,1);guard_knife=True
                        if index and index%10==0:
                            key_tap(21); time.sleep(.6)
                        x,y=targets[index%len(targets)]; index+=1
                        command(x,y,1024); time.sleep(.1); command(x,y); time.sleep(.12)
                    else:
                        time.sleep(.05)
                    if time.monotonic()-captured_at>=10:
                        command(*last_aim,capture=True)
                        captured_at=time.monotonic()
                        print(f"Progression context {context}; capture at shot {index}",flush=True)
                command(*last_aim)
                if guard_knife:event("key",knife_key,0)
                print("Progression captures:",re.findall(r"debug screenshot: ([^\r\n]+)",read_log())[-8:],flush=True)
            print(f"Visible audible window remains until its {lifetime}-second timeout; inspect native-action acceptance and captures.",flush=True)
            return
        if args.input_probe:
            for _ in range(8):
                tap(wait=.8)  # Dismiss the complete tutorial sequence before testing firing.
            command(0,0,4096); time.sleep(.15); command(0,0)
            time.sleep(1); command(0,0,capture=True)  # Observe actual pause screen and context.
            command(0,0,4096); time.sleep(.15); command(0,0); time.sleep(1)
            for _ in range(20):
                command(0,0,1024)  # Original B fire; empty magazine exercises the game's own reload.
                time.sleep(.08)
                command(0,0)
                time.sleep(.3)
            command(0,0,capture=True)
            time.sleep(1)
        for index in range(16):
            tap(wait=3, capture=index in (0, 4, 8))  # Capture milestones; PNG readback distorts frame/audio timing.
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
