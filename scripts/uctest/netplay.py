"""Two real game harnesses: direct-IP coop, native controls, hashes and disconnect.

Run from scripts: python -m uctest.netplay [--port 27889] [--speed 8].
All input uses the existing test server; saves/config stay in each run directory.
"""
import argparse
import contextlib
import csv
import json
import re
import time
import statistics
import hashlib
import math
import os

from .client import Game, ROOT, StepFailed, GUEST_CPU, NETPLAY_CLIENT_CPU
from .menus import screen_of
from . import steps
from .netem import DelayRelay


def record_rates(report, phase, rates, lower=.95, upper=1.05):
    failures = report.setdefault("performance_failures", [])
    passed = True
    for peer, ratio in rates.items():
        if lower <= ratio <= upper:
            continue
        failure = {"phase": phase, "peer": peer, "simulation_ratio": ratio,
                   "min_ratio": lower, "max_ratio": upper}
        failures.append(failure)
        report["ok"] = False
        passed = False
        print("PERFORMANCE FAILURE: " + json.dumps(failure), flush=True)
    return passed


def simulation_seconds(frame):
    return frame["simulation_seconds"] if "simulation_seconds" in frame else frame["game_seconds"] if "game_seconds" in frame else frame["retraces"]/60


def telemetry_rates(initial, final, wall_seconds):
    elapsed = (final["sample_time_ns"]-initial["sample_time_ns"])/1e9 if "sample_time_ns" in initial and "sample_time_ns" in final else wall_seconds
    if elapsed <= 0:
        raise ValueError("telemetry samples must advance")
    return {"fps": round((final["presents"]-initial["presents"])/elapsed, 2),
            "first_present": initial["presents"], "last_present": final["presents"],
            "simulation_ratio": round((simulation_seconds(final)-simulation_seconds(initial))/elapsed, 3),
            "game_seconds_start": initial.get("game_seconds"), "game_seconds_end": final.get("game_seconds"),
            "sample_seconds": elapsed}


def measurement_coverage(report, phase, peer, timing, start, end):
    samples = [(at, frame) for at, frame in timing if start <= at <= end]
    requested = end-start
    span = first_delay = last_age = None
    if samples:
        first_at, first = samples[0]
        last_at, last = samples[-1]
        span = (last["sample_time_ns"]-first["sample_time_ns"])/1e9 if "sample_time_ns" in first and "sample_time_ns" in last else last_at-first_at
        first_delay, last_age = first_at-start, end-last_at
    passed = bool(samples) and first_delay <= .25 and last_age <= .25 and abs(span-requested) <= .5
    measurement = {"ok": passed, "source_span_seconds": span, "requested_seconds": requested,
                   "first_arrival_delay_seconds": first_delay, "last_arrival_age_seconds": last_age,
                   "endpoint_tolerance_seconds": .25}
    if not passed:
        failure = {"phase": phase, "peer": peer, "kind": "measurement", "measurement": measurement}
        report.setdefault("performance_failures", []).append(failure)
        report["ok"] = False
        print("MEASUREMENT FAILURE: " + json.dumps(failure), flush=True)
    return measurement


def applied_caps(host, client, host_cap, client_cap):
    games, caps = (host, client), (host_cap, client_cap)
    before = [game.frame["sample_time_ns"] for game in games]
    for game, cap in zip(games, caps):
        game.set_options(fps_limit=cap)
    for game, cap, previous in zip(games, caps, before):
        game.wait(lambda frame: frame.get("fps_limit") == cap and frame["sample_time_ns"] > previous,
                  seconds=10, awaited=f"fresh telemetry with applied FPS ceiling {cap}")
    return dict(host.frame), dict(client.frame)


def latency_profiles(host, client, relay, ammo, rtt=None, *, report):
    results = []
    for delay, jitter in ((0, 0), (20, 5), (50, 10), (100, 20)):
        if rtt is not None and delay*2 != rtt:
            continue
        relay.set_profile(delay / 1000, jitter / 1000)
        start = time.perf_counter()
        initial = dict(host.frame)
        client_initial = dict(client.frame)
        # Wall time is intentional: measure how much simulation survives a fixed network delay.
        host.wait(lambda _: time.perf_counter() - start >= 10, seconds=20, awaited="network measurement")
        elapsed = time.perf_counter() - start
        final = dict(host.frame)
        client_final = dict(client.frame)
        host_rates = telemetry_rates(initial, final, elapsed)
        client_rates = telemetry_rates(client_initial, client_final, elapsed)
        with host._cond:
            host_timing = list(host.timing)
        with client._cond:
            client_timing = list(client.timing)
        gaps = present_intervals(host_timing, start, start+elapsed)
        response = []
        for index, game in enumerate((host, client)):
            before = ammo()[index]
            assert before > 0, "measurement requires loaded magazines"
            began = time.perf_counter()
            game._input({"cmd": "input", "hold_frames": 3, "mouse": {"button": 3, "down": True}}, wait=False)
            host.wait(lambda _: ammo()[index] < before, seconds=10, awaited="network shot response")
            response.append(round((time.perf_counter()-began)*1000, 1))
            host.advance(6)
        result = {"one_way_ms": delay, "jitter_ms": jitter, "nominal_rtt_ms": delay*2,
                  "wall_seconds": round(elapsed, 3),
                  "simulation_ratio": host_rates["simulation_ratio"],
                  "input_hz": round((final["n"]-initial["n"])/host_rates["sample_seconds"], 2),
                  "host_fps": host_rates["fps"], "client_fps": client_rates["fps"],
                  "host_sample_seconds": host_rates["sample_seconds"], "client_sample_seconds": client_rates["sample_seconds"],
                  "client_simulation_ratio": client_rates["simulation_ratio"],
                  "host_simulation_windows": simulation_windows(host_timing, start, start+elapsed),
                  "client_simulation_windows": simulation_windows(client_timing, start, start+elapsed),
                  "frame_gap_p50_ms": gaps["p50_ms"], "frame_gap_p95_ms": gaps["p95_ms"],
                  "frame_gap_p99_ms": gaps["p99_ms"], "frame_gap_max_ms": gaps["max_ms"],
                  "shot_response_ms": response}
        results.append(result)
        relay.check()
        print(json.dumps(result), flush=True)
        if "game_seconds" in initial:
            result["performance_ok"] = record_rates(report, f"baseline RTT {delay*2} ms",
                {"host": result["simulation_ratio"], "client": result["client_simulation_ratio"]})
        result["measurement"] = {peer: measurement_coverage(report, f"baseline RTT {delay*2} ms", peer, timing, start, start+elapsed)
                                 for peer, timing in (("host", host_timing), ("client", client_timing))}
        result["performance_ok"] = result.get("performance_ok", True) and all(value["ok"] for value in result["measurement"].values())
        windows_ok = [record_windows(report, f"baseline RTT {delay*2} ms", peer, result[f"{peer}_simulation_windows"])
                      for peer in ("host", "client")]
        result["performance_ok"] = result["performance_ok"] and all(windows_ok)
    relay.set_profile(0)
    return results


def word(game, address):
    return int(game.request({"cmd": "peek", "addr": address, "size": 4})["hex"], 16)


def present_intervals(timing, start, end):
    previous = None
    gaps = []
    unobserved = 0
    for at, frame in timing:
        if not start <= at <= end:
            continue
        presents = frame["presents"]
        sampled = frame.get("sample_time_ns", at*1e9)
        if previous is not None:
            if presents <= previous[1] or sampled <= previous[0]:
                continue
            gaps.append((sampled-previous[0])/1e6)
            unobserved += presents-previous[1]-1
        previous = sampled, presents
    gaps.sort()
    # Harness frame events observe presents; gaps can include several presents.
    return {"p50_ms": round(statistics.median(gaps), 1) if gaps else None,
            "p95_ms": round(gaps[min(len(gaps)-1, int(len(gaps)*.95))], 1) if gaps else None,
            "p99_ms": round(gaps[min(len(gaps)-1, int(len(gaps)*.99))], 1) if gaps else None,
            "max_ms": round(max(gaps), 1) if gaps else None,
            "samples": len(gaps), "unobserved_presents": unobserved}


def simulation_windows(timing, start, end):
    anchor = None
    last_at = None
    rates = []
    windows = []
    for at, frame in timing:
        if not start <= at <= end:
            continue
        sampled = frame.get("sample_time_ns", at*1e9)
        if last_at is not None and sampled <= last_at:
            continue
        last_at = sampled
        seconds = simulation_seconds(frame)
        if anchor is None:
            anchor = sampled, seconds, frame.get("game_seconds"), at
        elif sampled-anchor[0] >= 1e9:
            ratio = (seconds-anchor[1])*1e9/(sampled-anchor[0])
            rates.append(ratio)
            windows.append({"start_time_ns": anchor[0], "end_time_ns": sampled,
                            "start_arrival": anchor[3], "end_arrival": at,
                            "simulation_seconds_start": anchor[1], "simulation_seconds_end": seconds,
                            "game_seconds_start": anchor[2], "game_seconds_end": frame.get("game_seconds"),
                            "simulation_ratio": ratio})
            anchor = sampled, seconds, frame.get("game_seconds"), at
    rates.sort()
    return {"min_ratio": round(rates[0], 3) if rates else None,
            "p05_ratio": round(rates[min(len(rates)-1, int(len(rates)*.05))], 3) if rates else None,
            "max_ratio": round(rates[-1], 3) if rates else None,
            "below_0_95": sum(rate < .95 for rate in rates), "count": len(rates), "windows": windows}


def record_windows(report, phase, peer, summary):
    # Apply the existing simulation bounds to each source-time window, before rounding.
    bad = [window for window in summary["windows"] if not .95 <= window["simulation_ratio"] <= 1.05]
    passed = bool(summary["count"]) and not bad
    if not passed:
        failure = {"phase": phase, "peer": peer, "kind": "simulation_windows",
                   "min_ratio": .95, "max_ratio": 1.05, "failed_windows": bad,
                   "window_count": summary["count"]}
        report.setdefault("performance_failures", []).append(failure)
        report["ok"] = False
        print("WINDOW PERFORMANCE FAILURE: " + json.dumps(failure), flush=True)
    return passed


def native_stress_intervals(report, games):
    stress = report.get("network_stress")
    if stress is None:
        return
    for peer, game in zip(("host", "client"), games):
        measured = stress[peer]
        first, last = measured["first_present"], measured["last_present"]
        # CSV starts with the interval ending at presentation 2. Read only after exit.
        try:
            with (game.run_dir/"frames.csv").open(newline="") as stream:
                rows = list(csv.DictReader(stream))
            selected = rows[max(0, first-1):max(0, last-1)]
            gaps = [float(row["interval_ms"]) for row in selected]
            valid = len(gaps) == last-max(first, 1) and bool(gaps) and all(math.isfinite(gap) and gap > 0 for gap in gaps)
            error = None if valid else "missing or invalid native presentation intervals"
        except (OSError, ValueError, KeyError, TypeError, csv.Error) as exc:
            gaps, valid, error = [], False, str(exc)
        gaps.sort()
        stats = {"samples": len(gaps), "first_present": first, "last_present": last,
                 "p50_ms": statistics.median(gaps) if gaps else None,
                 "p99_ms": gaps[min(len(gaps)-1, int(len(gaps)*.99))] if gaps else None,
                 "max_ms": max(gaps) if gaps else None, "max_allowed_ms": 50, "p99_allowed_ms": 16.7,
                 "over_50_ms": sum(gap > 50 for gap in gaps), "measurement_error": error}
        # A 50ms native gap already spans three 60Hz periods; averages hide visible freezes.
        stats["ok"] = valid and max(gaps) <= 50 and stats["p99_ms"] <= 16.7
        measured["native_present_intervals"] = stats
        if not stats["ok"]:
            failure = {"phase": f"stress {stress['profile']}", "peer": peer,
                       "kind": "native_present_intervals", **stats}
            report.setdefault("performance_failures", []).append(failure)
            report["ok"] = stress["performance_ok"] = False
            print("NATIVE PRESENT FAILURE: " + json.dumps(failure), flush=True)


def rollback_log(log):
    hashes = {int(frame): value for frame, value in re.findall(r"\[netplay\] frame (\d+) hash ([0-9A-F]+)", log)}
    counters = re.findall(r"\[netplay\] rollback count (\d+) replayed (\d+) max_depth (\d+) confirmed (\d+) head (\d+)", log)
    latest = dict(zip(("count", "replayed", "max_depth", "confirmed", "head"), map(int, counters[-1]))) if counters else None
    return hashes, latest


def input_consumption_latency(timing, pending, start, end):
    # Includes IPC, configured hold duration and release consumption; not press-to-photon latency.
    selected = [(received-sent)*1000 for sent, received, accepted in timing if start <= sent <= end and accepted]
    selected.sort()
    ages = [end-sent for sent in pending if start <= sent <= end]
    return {"samples": len(selected),
            "p50_ms": round(statistics.median(selected), 1) if selected else None,
            "p95_ms": round(selected[min(len(selected)-1, int(len(selected)*.95))], 1) if selected else None,
            "max_ms": round(max(selected), 1) if selected else None,
            "rejected": sum(not accepted for sent, _, accepted in timing if start <= sent <= end),
            "pending_at_end": len(ages), "oldest_pending_ms": round(max(ages)*1000, 1) if ages else None}


def unstable_network(host, client, relay, report, profile, rtt, packet_loss=None):
    delay = rtt / 2000
    jitter = {0: 0, 40: .005, 100: .010, 200: .020}[rtt]
    loss, recovery = {"loss1": (.01, .1), "loss5": (.05, .2), "spike": (0, 0)}[profile]
    if packet_loss:
        loss = recovery = 0
    result = report["network_stress"] = {
        "profile": profile, "model": "WinDivert TCP/UDP drops and IP delay" if packet_loss else "UDP datagram loss and TCP recovery stalls",
        "nominal_rtt_ms": rtt, "jitter_ms": jitter*1000,
        "loss_rate": packet_loss.percent/100 if packet_loss else loss, "retransmit_delay_ms": recovery*1000,
        "spike_extra_one_way_ms": 200 if profile == "spike" else 0,
        "completed": False,
    }
    initial, client_initial = dict(host.frame), dict(client.frame)
    games = (host, client)
    before_logs = [rollback_log((game.run_dir/"run.log").read_text(errors="replace")) for game in games]
    first_hash = max((max(hashes, default=-1) for hashes, _ in before_logs), default=-1)
    inputs = [{"aim": 0, "shots": 0, "reloads": 0} for _ in games]
    result["inputs"] = inputs
    counters = relay.stats()
    started = time.perf_counter()
    phase = None
    next_input = 0
    input_step = 0

    def sample(_):
        nonlocal phase, next_input, input_step
        elapsed = time.perf_counter()-started
        relay.check()
        if packet_loss:
            packet_loss.check()
        spike = profile == "spike" and int(elapsed) % 8 == 3
        if spike != phase:
            relay.set_profile(0 if packet_loss else delay+(.2 if spike else 0), 0 if packet_loss else jitter,
                              loss_rate=loss, retransmit_delay=recovery)
            if packet_loss:
                packet_loss.set_delay(delay+(.2 if spike else 0), jitter)
            phase = spike
        if elapsed < 30 and elapsed >= next_input:
            for index, game in enumerate(games):
                if game.frame.get("context") != "gameplay":
                    continue
                angle = input_step*.7 + index*math.pi
                game.aim(.65*math.sin(angle), .4*math.cos(angle), wait=False)
                inputs[index]["aim"] += 1
                if input_step % 12 == 11:
                    game.key(steps.KEYS["reload"], frames=3, wait=False)
                    inputs[index]["reloads"] += 1
                elif input_step % 2 == 0:
                    game._input({"cmd": "input", "hold_frames": 3, "mouse": {"button": 3, "down": True}}, wait=False)
                    inputs[index]["shots"] += 1
            input_step += 1
            next_input = elapsed+.25
        return elapsed >= 30

    print(f"Unstable network: {profile}, 30 seconds, {result['model']}", flush=True)
    try:
        sample(None)
        host.wait(sample, seconds=40, awaited=f"unstable network {profile}")
    finally:
        ended = time.perf_counter()
        elapsed = ended-started
        relay.set_profile(delay, jitter)
        peers = []
        logs = []
        for index, game in enumerate(games):
            with game._cond:
                final, timing = dict(game.frame), list(game.timing)
                input_latency = input_consumption_latency(list(game.input_timing), list(game._input_sent.values()), started, ended)
            hashes, rollback = rollback_log((game.run_dir/"run.log").read_text(errors="replace"))
            logs.append(hashes)
            baseline = before_logs[index][1]
            if rollback is not None:
                rollback = {**rollback,
                            "count": rollback["count"]-(baseline["count"] if baseline else 0),
                            "replayed": rollback["replayed"]-(baseline["replayed"] if baseline else 0),
                            "max_depth_scope": "session"}
            begin = (initial, client_initial)[index]
            peers.append({**telemetry_rates(begin, final, elapsed),
                          "input_consumption_latency": input_latency,
                          "measurement": measurement_coverage(report, f"stress {profile} RTT {rtt} ms", ("host", "client")[index], timing, started, ended),
                          "simulation_windows": simulation_windows(timing, started, ended),
                          "present_intervals": present_intervals(timing, started, ended), "rollback": rollback})
        common = sorted(n for n in logs[0].keys() & logs[1].keys() if n > first_hash)
        mismatches = [n for n in common if logs[0][n] != logs[1][n]]
        result["confirmed_hashes"] = {"matching": len(common)-len(mismatches), "mismatched_frames": mismatches,
                                      "first_frame": common[0] if common else None,
                                      "last_frame": common[-1] if common else None}
        result["host"], result["client"] = peers
        result.update(wall_seconds=round(elapsed, 3),
                      host_fps=peers[0]["fps"], client_fps=peers[1]["fps"],
                      host_simulation_ratio=peers[0]["simulation_ratio"], client_simulation_ratio=peers[1]["simulation_ratio"])
        result["elapsed_seconds"] = round(elapsed, 3)
        result["relay"] = {k: v-counters[k] for k, v in relay.stats().items()}
        if packet_loss:
            result["ip_loss"] = packet_loss.stats()
            result["ip_filter"] = packet_loss.filter
    result["performance_ok"] = record_rates(report, f"stress {profile} RTT {rtt} ms",
        {"host": result["host_simulation_ratio"], "client": result["client_simulation_ratio"]})
    result["performance_ok"] = result["performance_ok"] and all(peer["measurement"]["ok"] for peer in peers)
    windows_ok = [record_windows(report, f"stress {profile} RTT {rtt} ms", name, peer["simulation_windows"])
                  for name, peer in zip(("host", "client"), peers)]
    result["performance_ok"] = result["performance_ok"] and all(windows_ok)
    assert common and not mismatches, "stress confirmed hashes missing or disagree at the same frame"
    assert all(peer["rollback"] is not None for peer in peers), "rollback counters missing from run.log"
    assert all(counts["aim"] and counts["shots"] for counts in inputs), "stress needs gameplay input from both players"
    if packet_loss:
        assert packet_loss.stats()["udp_packets_dropped"] > 0, "IP loss filter dropped no gameplay UDP packets"
    result["completed"] = True


def coop_menu(host, client):
    steps.skip_movies(host, seconds=60)
    for target in ("title_start", "save_slot_1", "scenario_train", "chapter_1"):
        steps.select(host, target)
    # Coop character selection waits for both players; the equipment page is P1 then P2.
    # The page object appears during its entrance animation, before it accepts selections.
    host.advance(120)
    for _ in range(30):
        if screen_of(host.frame) == "loadout":
            break
        for game, x in ((host, -.3), (client, .3)):
            game.aim(x, -.1, settle_frames=3)
            game.press("A", 12, x, -.1)
            # Selection animates on the shared page; its other player's next click is gated too.
            host.advance(30)
        host.advance(30)
    if screen_of(host.frame) != "loadout":
        host.fail("both characters must be selected", "loadout")
    for _ in range(30):
        if screen_of(host.frame) == "start_dialog":
            break
        for game in (host, client):
            game.aim(.625, .4, settle_frames=3)
            game.press("A", 12, .625, .4)
        host.advance(30)
    assert screen_of(host.frame) == "start_dialog", "both loadouts must be confirmed"
    # The final equipment owner also owns the start dialog (P2 in coop).
    for _ in range(30):
        if screen_of(host.frame) == "mission":
            break
        for game in (client, host):
            game.aim(-.35, .2, settle_frames=3)
            game.press("A", 12, -.35, .2)
        host.advance(30)
    assert screen_of(host.frame) == "mission", "coop start dialog must close"
    steps.wait_gameplay(host)


def run(args):
    folder = ROOT / "build" / "autotest" / f"netplay-{time.time_ns()}"
    folder.mkdir(parents=True)
    report = {"ok": False, "performance_failures": [], "speed": args.speed, "guest_cpus": [GUEST_CPU, NETPLAY_CLIENT_CPU]}
    print(folder, flush=True)
    try:
        with contextlib.ExitStack() as stack:
            relay = DelayRelay(args.port) if args.latency_test else None
            if relay:
                stack.callback(relay.close)
                if args.latency_rtt is not None:
                    jitter = {0: 0, 40: 5, 100: 10, 200: 20}[args.latency_rtt]
                    relay.set_profile(args.latency_rtt / 2000, jitter / 1000)
            host = stack.enter_context(Game(exe=args.exe, run_dir=folder/"host", speed=args.speed, seconds=180+args.soak_seconds,
                                            game_args=["--net-host", "--net-port", str(args.port)]))
            if args.connect_delay:
                time.sleep(args.connect_delay)
                assert host.process.poll() is None, "host must wait for the peer's slower startup"
            if args.different_saves:
                personal = folder/"client"/"saves"
                personal.mkdir(parents=True)
                (personal/"personal-save").write_bytes(b"client private progress, never replace")
                before_saves = {str(p.relative_to(personal)): hashlib.sha256(p.read_bytes()).hexdigest() for p in personal.rglob("*") if p.is_file()}
            client = stack.enter_context(Game(exe=args.exe, run_dir=folder/"client", speed=args.speed, seconds=180+args.soak_seconds,
                                              game_args=["--net-join", "127.0.0.1", "--net-port", str(relay.port if relay else args.port)]))
            coop_menu(host, client)
            pointers = [word(host, 0x806CA620-27464+i*4) for i in range(2)]
            assert all(0x80000000 <= p < 0x81800000 for p in pointers) and pointers[0] != pointers[1]
            assert [word(host, p+0x790) for p in pointers] == [0, 1]
            report["players"] = pointers
            report["weapon_words"] = [[word(host, p+offset) for offset in (0x4C8, 0x4CC, 0x4D4, 0x4D8, 0x4DC)] for p in pointers]
            assert report["weapon_words"][1][-1] != 0xFFFFFFFF, "P2 must join before character confirmation"
            print("coop gameplay: two players", flush=True)
            ammo = lambda: [word(host, p+0x4D4) for p in pointers]
            report["controls"] = []
            for index, game in enumerate((host, client)):
                steps.wait_gameplay(host, stable_ticks=3)
                game.aim(-.5 if index == 0 else .5, -.3, settle_frames=3)
                before = ammo()
                if before[index] == 0:
                    steps.perform(game, "reload")
                    host.wait(lambda _: ammo()[index] > 0, seconds=10, awaited=f"P{index+1} initial magazine")
                    before = ammo()
                # Physical right mouse: exercises native context mapping as well as network routing.
                game._input({"cmd": "input", "hold_frames": 3, "mouse": {"button": 3, "down": True}}, wait=True)
                host.advance(15)
                after = ammo()
                assert after[index] < before[index] and after[1-index] == before[1-index], (index, before, after)
                reload_event = steps.perform(game, "reload")
                host.wait(lambda _: ammo()[index] == before[index], seconds=10, awaited=f"P{index+1} reloaded")
                knife = steps.perform(game, "knife")
                report["controls"].append({"player": index+1, "before": before, "shot": after, "reload": reload_event, "knife": knife})
                print(f"P{index+1}: aim, fire, reload, knife passed", flush=True)
            if relay:
                report["latency_profiles"] = latency_profiles(host, client, relay, ammo, args.latency_rtt, report=report)
            if args.network_stress:
                with contextlib.ExitStack() as impairment:
                    packet_loss = None
                    if args.ip_loss:
                        from .ip_loss import PacketLoss
                        packet_loss = PacketLoss(args.windivert_dll, args.port, args.ip_loss,
                                                 args.latency_rtt/2000, jitter/1000)
                        impairment.callback(packet_loss.close)
                    unstable_network(host, client, relay, report, args.network_stress, args.latency_rtt, packet_loss)
            if args.soak_seconds:
                print(f"Checking synchronization for another {args.soak_seconds} seconds...", flush=True)
                assert relay and args.latency_rtt is not None
                relay.set_profile(args.latency_rtt / 2000, jitter / 1000)
                began = time.monotonic()
                initial = host.frame["n"]
                host.wait(lambda _: time.monotonic()-began >= args.soak_seconds,
                          seconds=args.soak_seconds+10, awaited="sustained network simulation")
                report["soak_seconds"] = round(time.monotonic()-began, 3)
                report["soak_input_frames"] = host.frame["n"]-initial
                assert report["soak_input_frames"] >= args.soak_seconds*30, "sustained simulation stalled"
                relay.check()
            report["fps_cap_phases"] = []
            for host_cap, client_cap in ((200, 30), (30, 200)):
                print(f"Checking independent FPS ceilings: host {host_cap}, client {client_cap}; both games must remain 1x.", flush=True)
                initial, client_initial = applied_caps(host, client, host_cap, client_cap)
                changed_at = time.monotonic()
                host.wait(lambda frame: simulation_seconds(frame)-simulation_seconds(initial) >= 6 and
                          simulation_seconds(client.frame)-simulation_seconds(client_initial) >= 6,
                          seconds=30, awaited=f"six game seconds with FPS ceilings {host_cap}/{client_cap}")
                elapsed = time.monotonic()-changed_at
                final, client_final = dict(host.frame), dict(client.frame)
                host_rates = telemetry_rates(initial, final, elapsed)
                client_rates = telemetry_rates(client_initial, client_final, elapsed)
                phase = {"host_fps_limit": host_cap, "client_fps_limit": client_cap,
                         "wall_seconds": round(elapsed, 3),
                         "host_simulation_ratio": host_rates["simulation_ratio"], "client_simulation_ratio": client_rates["simulation_ratio"],
                         "host_fps": host_rates["fps"], "client_fps": client_rates["fps"],
                         "host_sample_seconds": host_rates["sample_seconds"], "client_sample_seconds": client_rates["sample_seconds"]}
                report["fps_cap_phases"].append(phase)
                report["fps_change_simulation_ratio"] = phase["host_simulation_ratio"]
                print(json.dumps(phase), flush=True)
                phase["performance_ok"] = record_rates(report, f"FPS ceilings {host_cap}/{client_cap}",
                    {"host": phase["host_simulation_ratio"], "client": phase["client_simulation_ratio"]}, .98, 1.02)
            host.set_options(fps_limit=0)
            client.set_options(fps_limit=0)
            report["local_fps_change"] = all(phase["performance_ok"] for phase in report["fps_cap_phases"])
            # Remote shutdown must stop the waiting peer without a deadlock or orphan game.
            began = time.monotonic()
            client_log = client.finish()
            assert client.process.returncode == 0
            host.process.wait(15)
            assert host.process.returncode == 5, host.process.returncode
            host_log = (host.run_dir/"run.log").read_text(errors="replace")
            report["disconnect_seconds"] = round(time.monotonic()-began, 3)
            native_stress_intervals(report, (host, client))
            hashes = lambda log: dict(re.findall(r"\[netplay\] frame (\d+) hash ([0-9A-F]+)", log))
            left, right = hashes(host_log), hashes(client_log)
            common = left.keys() & right.keys()
            assert len(common) >= 5 and all(left[n] == right[n] for n in common)
            assert "desync" not in host_log.lower() and "desync" not in client_log.lower()
            report["matching_hashes"] = len(common)
            pipelines = [re.search(r"input pipeline: (\d+) frames; max RTT ([\d.]+) ms", log) for log in (host_log, client_log)]
            assert all(pipelines), "both peers must negotiate their input delay"
            assert pipelines[0].groups() == pipelines[1].groups(), "input delay negotiation disagreed"
            report["input_delay_frames"] = int(pipelines[0][1])
            report["startup_max_rtt_ms"] = float(pipelines[0][2])
            buffer_rtts = [re.search(r"input buffer RTT p75 ([\d.]+) ms", log) for log in (host_log, client_log)]
            if any(buffer_rtts):
                assert all(buffer_rtts) and buffer_rtts[0][1] == buffer_rtts[1][1], "input buffer RTT negotiation disagreed"
                report["startup_buffer_rtt_ms"] = float(buffer_rtts[0][1])
            report["last_checked_input_frame"] = max(map(int, common))
            if args.different_saves:
                after_saves = {str(p.relative_to(personal)): hashlib.sha256(p.read_bytes()).hexdigest() for p in personal.rglob("*") if p.is_file()}
                assert after_saves == before_saves, "client personal saves changed"
                temporary = re.search(r"\[netplay\] temporary saves: (.+)", client_log)
                assert temporary, "client must report its session save directory"
                from pathlib import Path
                assert not Path(temporary[1]).exists(), "session saves not cleaned up"
                report["client_saves_preserved"] = report["temporary_saves_removed"] = True
        # Deliberately incompatible simulation settings must fail before any input frame.
        with contextlib.ExitStack() as stack:
            host = stack.enter_context(Game(exe=args.exe, run_dir=folder/"mismatch-host", speed=args.speed, seconds=45,
                                            game_args=["--net-host", "--net-port", str(args.port+1)]))
            client = stack.enter_context(Game(exe=args.exe, run_dir=folder/"mismatch-client", speed=args.speed, seconds=45, god=True,
                                              game_args=["--net-join", "127.0.0.1", "--net-port", str(args.port+1)]))
            for game in (host, client):
                game.process.wait(30)
                log = (game.run_dir/"run.log").read_text(errors="replace")
                assert game.process.returncode == 5 and "Network startup mismatch" in log
                assert "[netplay] frame" not in log
            assert not list(client.run_dir.glob(".netplay-session-*")), "failed startup left session saves"
            report["startup_mismatch_rejected"] = True
        report["functional_checks_completed"] = True
        report["ok"] = not report["performance_failures"]
    except Exception as error:
        report["error"] = str(error)
        if isinstance(error, StepFailed):
            report["artifacts"] = error.artifacts
    (folder/"report.json").write_text(json.dumps(report, indent=2), encoding="utf8")
    print(json.dumps(report), flush=True)
    return 0 if report["ok"] else 1


def main():
    # The FPS and speed checks need a fixed tick: a tick change inside a measurement window would move the ratios.
    os.environ.setdefault("UC_NET_TICK_AUTO", "0")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", default="dist/uc.exe")
    parser.add_argument("--port", type=int, default=27889, choices=range(1024, 65535), metavar="1024..65534")
    parser.add_argument("--speed", type=int, default=8, choices=range(1, 33), metavar="1..32")
    parser.add_argument("--connect-delay", type=int, default=0, choices=range(31), metavar="0..30", help="delay client startup to exercise the network wait watchdog")
    parser.add_argument("--latency-test", action="store_true", help="measure RTT 0/40/100/200ms with jitter; requires --speed 1")
    parser.add_argument("--different-saves", action="store_true", help="start client with unrelated private saves; verify import isolation and cleanup")
    parser.add_argument("--latency-rtt", type=int, choices=(0, 40, 100, 200), help="one profile per fresh game pair for comparable gameplay")
    parser.add_argument("--soak-seconds", type=int, default=0, choices=range(121), metavar="0..120", help="continue checking simulation after measurements, including death/menu transitions")
    parser.add_argument("--network-stress", choices=("loss1", "loss5", "spike"), help="30s active two-player input under UDP loss/TCP recovery stalls or +200ms one-way bursts")
    parser.add_argument("--ip-loss", type=int, choices=(1, 5), help="actual IP loss during stress; requires approved WinDivert driver and Administrator")
    parser.add_argument("--windivert-dll", help="absolute path to the official WinDivert 2.2 x64 DLL")
    args = parser.parse_args()
    if bool(args.ip_loss) != bool(args.windivert_dll):
        parser.error("--ip-loss and --windivert-dll must be supplied together")
    if args.ip_loss and not args.network_stress:
        args.network_stress = f"loss{args.ip_loss}"
    if args.network_stress and args.latency_rtt is None:
        args.latency_rtt = 100
    if args.latency_rtt is not None:
        args.latency_test = True
    if args.latency_test and args.speed != 1:
        parser.error("--latency-test requires --speed 1")
    if args.soak_seconds and args.latency_rtt is None:
        parser.error("--soak-seconds requires --latency-rtt")
    if args.latency_test and args.latency_rtt is None:
        for rtt in (0, 40, 100, 200):
            if run(argparse.Namespace(**{**vars(args), "latency_rtt": rtt})):
                return 1
        return 0
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
