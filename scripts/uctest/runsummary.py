"""Summaries of a run's once-a-second profile lines: python -m uctest.runsummary [run_dir] [seconds]  (newest build/autotest run by default).

fps_summary: presents per second and the interval between presents ([fps], [frametime]); gpu_summary: GPU time per category per second ([gpu-sum], the motion blur pass of
[gpu], the render thread's [perf] render). Both use the last `seconds` lines of each series (the measured window of a perf run)."""
import re
import statistics
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FPS = re.compile(r"^\[fps\] window_ms [0-9.]+ retraces \d+:\d+ presents \d+ fps ([0-9.]+)", re.M)
FRAMETIME = re.compile(r"^\[frametime\] n \d+ avg [0-9.]+ p50 ([0-9.]+) p99 ([0-9.]+) max [0-9.]+ ms", re.M)
GPU_SUM = re.compile(r"^\[gpu-sum\] per second: scene draws ([0-9.]+) ms, efb draws ([0-9.]+) ms, texture copies ([0-9.]+) ms, bloom ([0-9.]+) ms, upscale ([0-9.]+) ms", re.M)
BLUR = re.compile(r"^\[gpu\] passes:.*?motion blur ([0-9.]+) ms", re.M)
RENDER = re.compile(r"^\[perf\] presents \d+ render ([0-9.]+) ms", re.M)
PROFILE_WINDOW = re.compile(r"^\[fps\] window_ms (\d+(?:\.\d+)?) retraces (\d+):(\d+) ", re.M)


def measurement_log(log, from_retrace, to_retrace):
    """Complete profile seconds inside the measured retrace interval, with their pacing/GPU lines."""
    windows = list(PROFILE_WINDOW.finditer(log))
    blocks = []
    for i, window in enumerate(windows):
        ms, first, last = window.groups()
        if float(ms) >= 1000 and from_retrace <= int(first) <= int(last) <= to_retrace:
            end = windows[i+1].start() if i+1 < len(windows) else len(log)
            blocks.append(log[window.start():end])
    return "".join(blocks)


def _last(pattern, log, seconds):
    return pattern.findall(log)[-seconds:]


def fps_summary(log, seconds):
    fps = [float(x) for x in _last(FPS, log, seconds)]
    frames = [(float(p50), float(p99)) for p50, p99 in _last(FRAMETIME, log, seconds)]
    if not fps or not frames:
        return {}
    return {"fps_mean": statistics.fmean(fps), "fps_min": min(fps), "fps_max": max(fps),
            "frame_p50": statistics.fmean(f[0] for f in frames), "frame_p99_mean": statistics.fmean(f[1] for f in frames), "frame_p99_worst": max(f[1] for f in frames)}


def gpu_summary(log, seconds):
    sums = [[float(v) for v in row] for row in _last(GPU_SUM, log, seconds)]
    if not sums:
        return {}
    scene, efb, copies, bloom, upscale = (statistics.fmean(column) for column in zip(*sums))
    blur = [float(x) for x in _last(BLUR, log, seconds)]
    render = [float(x) for x in _last(RENDER, log, seconds)]
    fps = fps_summary(log, seconds).get("fps_mean", 0.0)
    blur_frame = statistics.fmean(blur) if blur else 0.0
    return {"scene": scene, "efb": efb, "copies": copies, "bloom": bloom, "upscale": upscale, "blur_ms_frame": blur_frame,
            "render_ms_s": statistics.fmean(render) if render else 0.0, "gpu_total_ms_s": scene + efb + copies + bloom + upscale + blur_frame * fps}


def newest_run(folder=None):
    """The newest run folder of build/autotest, or None when there is none."""
    folder = folder or ROOT / "build" / "autotest"
    runs = sorted(folder.iterdir(), key=lambda p: p.stat().st_mtime) if folder.is_dir() else []
    return runs[-1] if runs else None


def format_summary(log, seconds):
    fps, gpu = fps_summary(log, seconds), gpu_summary(log, seconds)
    if not fps:
        return "no profile lines in the log (was the game run with --profile?)"
    text = (f"fps mean {fps['fps_mean']:.1f} min {fps['fps_min']:.0f} MAX {fps['fps_max']:.1f} | frame p50 {fps['frame_p50']:.1f} ms, p99 mean {fps['frame_p99_mean']:.1f} worst {fps['frame_p99_worst']:.1f} ms")
    if gpu:
        text += (f"\n  gpu ms/s: scene {gpu['scene']:.0f}, efb {gpu['efb']:.0f}, copies {gpu['copies']:.0f}, bloom {gpu['bloom']:.0f}, upscale {gpu['upscale']:.0f}, "
                 f"blur {gpu['blur_ms_frame']:.2f} ms/frame; total {gpu['gpu_total_ms_s']:.0f}; render thread {gpu['render_ms_s']:.0f}")
    return text


if __name__ == "__main__":
    run = Path(sys.argv[1]) if len(sys.argv) > 1 else newest_run()
    if run is None or not (run / "run.log").exists():
        sys.exit("no run with a run.log found (build/autotest is empty)")
    print(f"{run.name}: " + format_summary((run / "run.log").read_text(errors="replace"), int(sys.argv[2]) if len(sys.argv) > 2 else 8))
