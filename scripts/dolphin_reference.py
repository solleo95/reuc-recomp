"""Capture a DFF reference through Dolphin's scripting API without desktop input."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import subprocess
import time


def capture_script(output):
    return (
        "from dolphin import event, emulation\nfrom pathlib import Path\nimport json\n"
        f"out = Path({str(output)!r})\ncount = 0\n"
        "def frame(width, height, data):\n"
        "    global count\n"
        "    if count >= 6: return\n"
        "    if len(data) != width * height * 3: raise RuntimeError('Unexpected frame byte count')\n"
        "    (out / f'frame-{count}.ppm').write_bytes(f'P6\\n{width} {height}\\n255\\n'.encode() + data)\n"
        "    count += 1\n"
        "    if count == 6:\n"
        "        (out / 'complete.json').write_text(json.dumps({'frames': count, 'width': width, 'height': height}))\n"
        "        emulation.pause()\n"
        "event.on_framedrawn(frame)\n"
        "while True: await event.frameadvance()\n")


def main():
    import localenv  # noqa: F401  (reads local.env into the environment; imported here because tests load this file by path)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--dolphin", type=Path, default=os.environ.get("UC_DOLPHIN"), help="a Dolphin build with the scripting API (default: UC_DOLPHIN in local.env)")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--dump-efb", action="store_true", help="also dump every EFB copy (Dolphin DumpEFBTarget)")
    args = parser.parse_args()
    if args.dolphin is None:
        parser.error("pass --dolphin or set UC_DOLPHIN in local.env")
    capture = args.capture.resolve(strict=True)
    dolphin = args.dolphin.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    profile = output / "user"
    config = profile / "Config"
    config.mkdir(parents=True)
    (config / "Dolphin.ini").write_text(
        "[Core]\nGFXBackend = Vulkan\nCPUThread = False\n"
        "[Interface]\nConfirmStop = False\nUsePanicHandlers = False\nOnScreenDisplayMessages = False\nPauseOnFocusLost = False\n"
        "[Display]\nFullscreen = False\nRenderToMain = False\nRenderWindowWidth = 640\nRenderWindowHeight = 480\n"
        "[Analytics]\nEnabled = False\nPermissionAsked = True\n[AutoUpdate]\nUpdateTrack = \n", encoding="utf-8")
    (config / "GFX.ini").write_text(
        "[Settings]\nInternalResolution = 1\nAspectRatio = 2\nShowFPS = False\nDumpTextures = True\n"
        + ("DumpEFBTarget = True\n" if args.dump_efb else "") +
        "InternalResolutionFrameDumps = True\n[Hardware]\nVSync = False\n", encoding="utf-8")
    script = output / "capture.py"
    script.write_text(capture_script(output), encoding="utf-8")
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 4  # Visible, without activation.
    with (output / "process.log").open("wb") as log:
        process = subprocess.Popen([str(dolphin), "-b", "-u", str(profile), "-e", str(capture), "--script", str(script)],
                                   startupinfo=startup, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 45
            while not (output / "complete.json").exists():
                if process.poll() is not None:
                    raise RuntimeError(f"Dolphin exited {process.returncode}; see {output / 'process.log'}")
                if time.monotonic() >= deadline:
                    raise TimeoutError(f"Reference capture timed out; see {output}")
                time.sleep(0.1)
            result = json.loads((output / "complete.json").read_text())
            from PIL import Image
            for frame in range(result["frames"]):
                with Image.open(output / f"frame-{frame}.ppm") as image:
                    image.save(output / f"frame-{frame}.png")
            print(json.dumps(result), flush=True)
        finally:
            # Close only windows belonging to the process this invocation created.
            if process.poll() is None:
                callback_type = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_ssize_t)
                def close_window(hwnd, _):
                    pid = ctypes.c_ulong()
                    ctypes.windll.user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(pid))
                    if pid.value == process.pid:
                        ctypes.windll.user32.PostMessageW(ctypes.c_void_p(hwnd), 0x10, 0, 0)
                    return True
                ctypes.windll.user32.EnumWindows(callback_type(close_window), 0)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(timeout=5)


if __name__ == "__main__":
    main()
