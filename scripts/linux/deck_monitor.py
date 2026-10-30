#!/usr/bin/env python3
"""Runs ON the Steam Deck next to a game run: samples what the game uses, every 250 ms, into a CSV until `uc` has ended.

  python3 deck_monitor.py <out.csv>

Columns: t (s), then for the game process: total CPU % of one core (so 100 = one busy core), the busiest thread's name and its %, the second busiest thread's name and its %,
the mean and the highest clock of the 8 CPU threads (MHz), the GPU busy % (amdgpu gpu_busy_percent), the GPU shader clock (MHz) and the APU power (W) where the kernel tells.
"""
import glob
import os
import subprocess
import sys
import time

TICK = os.sysconf("SC_CLK_TCK")


def pid_of_uc():
    out = subprocess.run(["pgrep", "-x", "uc|uc_.*"], capture_output=True, text=True).stdout.split()
    return int(out[0]) if out else None


def thread_times(pid):
    times = {}
    for path in glob.glob(f"/proc/{pid}/task/*/stat"):
        try:
            text = open(path).read()
        except OSError:
            continue
        tid = path.split("/")[4]
        name = text[text.index("(") + 1:text.rindex(")")]
        fields = text[text.rindex(")") + 2:].split()
        times[tid] = (name, (int(fields[11]) + int(fields[12])) / TICK)   # utime + stime
    return times


def read(path, default=None):
    try:
        return open(path).read().strip()
    except OSError:
        return default


def gpu_paths():
    for card in sorted(glob.glob("/sys/class/drm/card*/device")):
        if os.path.exists(f"{card}/gpu_busy_percent"):
            return card
    return None


def main():
    out = open(sys.argv[1], "w")
    out.write("t,cpu_pct,top1,top1_pct,top2,top2_pct,top3,top3_pct,cpu_mean_mhz,cpu_max_mhz,gpu_busy_pct,gpu_sclk_mhz,power_w\n")
    card = gpu_paths()
    hwmon = glob.glob(f"{card}/hwmon/hwmon*/power1_average") if card else []
    pid = None
    deadline = time.time() + 180
    while pid is None and time.time() < deadline:   # the game starts a little after this script
        pid = pid_of_uc()
        time.sleep(0.2)
    if pid is None:
        return
    start = time.monotonic()
    previous, previous_t = thread_times(pid), time.monotonic()
    gone = 0
    while True:
        time.sleep(0.25)
        if not os.path.exists(f"/proc/{pid}"):
            gone += 1
            if gone > 8:
                break
            continue
        now = time.monotonic()
        current = thread_times(pid)
        dt = now - previous_t
        usage = sorted(((current[t][1] - previous[t][1]) / dt * 100, current[t][0]) for t in current if t in previous)[::-1]
        total = sum(u for u, _ in usage)
        previous, previous_t = current, now
        clocks = [int(read(p, "0")) / 1000 for p in glob.glob("/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_cur_freq")]
        busy = read(f"{card}/gpu_busy_percent", "") if card else ""
        sclk = ""
        if card:
            for line in (read(f"{card}/pp_dpm_sclk", "") or "").splitlines():
                if line.rstrip().endswith("*"):
                    sclk = line.split(":")[1].strip().split("M")[0]
        power = ""
        if hwmon:
            raw = read(hwmon[0], "")
            power = f"{int(raw) / 1e6:.1f}" if raw else ""
        tops = (usage + [(0, ""), (0, ""), (0, "")])[:3]
        out.write(f"{now - start:.2f},{total:.0f},{tops[0][1]},{tops[0][0]:.0f},{tops[1][1]},{tops[1][0]:.0f},{tops[2][1]},{tops[2][0]:.0f},"
                  f"{sum(clocks) / max(1, len(clocks)):.0f},{max(clocks, default=0):.0f},{busy},{sclk},{power}\n")
        out.flush()
    out.close()


main()
