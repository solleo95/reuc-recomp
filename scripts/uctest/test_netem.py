import socket
import json
import time
import unittest
import tempfile
import threading
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

from .netem import DelayRelay
from .client import Game
from .netplay import applied_caps, input_consumption_latency, measurement_coverage, native_stress_intervals, present_intervals, record_rates, record_windows, rollback_log, simulation_windows, telemetry_rates, unstable_network


class DatagramRelayTests(unittest.TestCase):
    def setUp(self):
        self.host = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.host.bind(("127.0.0.1", 0))
        self.host.settimeout(1)
        self.client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.client.bind(("127.0.0.1", 0))
        self.client.settimeout(1)
        self.relay = DelayRelay(self.host.getsockname()[1])
        self.address = ("127.0.0.1", self.relay.port)

    def tearDown(self):
        try:
            self.relay.close()
        finally:
            self.client.close()
            self.host.close()

    def test_bidirectional_no_loss(self):
        self.relay.set_profile(0.01, 0.005)
        for i in range(8):
            payload = bytes([i]) * 1152
            self.client.sendto(payload, self.address)
            received, source = self.host.recvfrom(65536)
            self.assertEqual(received, payload)
            self.assertEqual(source, self.address)
            self.host.sendto(payload[::-1] + b"reply", source)
            self.assertEqual(self.client.recvfrom(65536)[0], payload[::-1] + b"reply")
        self.assertEqual(self.relay.stats()["udp_datagrams"], 16)
        self.assertEqual(self.relay.stats()["udp_dropped"], 0)
        self.assertEqual(self.relay.stats()["udp_bytes"], 8 * (1152 * 2 + 5))
        self.relay.check()

    def test_full_loss_drops_both_directions(self):
        self.relay.set_profile(0, loss_rate=1, retransmit_delay=0.01)
        self.host.settimeout(0.15)
        self.client.settimeout(0.15)
        self.client.sendto(b"lost request", self.address)
        with self.assertRaises(socket.timeout):
            self.host.recvfrom(64)
        self.host.sendto(b"lost reply", self.address)
        with self.assertRaises(socket.timeout):
            self.client.recvfrom(64)
        self.assertEqual(self.relay.stats()["udp_dropped"], 2)
        self.relay.check()

    def test_close_discards_delayed_queue_and_joins(self):
        self.relay.set_profile(5)
        self.client.sendto(b"pending", self.address)
        deadline = time.monotonic() + 1
        while not self.relay.stats()["udp_datagrams"] and time.monotonic() < deadline:
            time.sleep(0.005)
        self.assertEqual(self.relay.stats()["udp_datagrams"], 1)
        started = time.monotonic()
        self.relay.close()
        self.assertLess(time.monotonic() - started, 1)
        self.assertFalse(self.relay._thread.is_alive())
        self.assertFalse(self.relay._udp_thread.is_alive())


class StressReportTests(unittest.TestCase):
    def test_measurement_coverage_allows_quarter_second_endpoints(self):
        timing = [(.25, {"sample_time_ns": 500000000000}), (9.75, {"sample_time_ns": 509500000000})]
        self.assertTrue(measurement_coverage({}, "baseline", "host", timing, 0, 10)["ok"])

    def test_measurement_coverage_rejects_late_start_and_short_source_span(self):
        report = {}
        with patch("builtins.print"):
            late = measurement_coverage(report, "baseline", "host", [(.3, {}), (10, {})], 0, 10)
            short = measurement_coverage(report, "stress", "client",
                [(0, {"sample_time_ns": 500000000000}), (10, {"sample_time_ns": 508000000000})], 0, 10)
        self.assertFalse(late["ok"])
        self.assertFalse(short["ok"])
        self.assertFalse(report["ok"])
        self.assertEqual([failure["kind"] for failure in report["performance_failures"]], ["measurement", "measurement"])

    def test_source_sample_deltas_ignore_delivery_delay_but_keep_slowdown(self):
        initial = {"sample_time_ns": 9000000000000000000, "game_seconds": 3, "presents": 60}
        final = {"sample_time_ns": initial["sample_time_ns"]+8000000000, "game_seconds": 11, "presents": 540}
        self.assertEqual(telemetry_rates(initial, final, 10),
                         {"fps": 60, "simulation_ratio": 1, "sample_seconds": 8,
                          "first_present": 60, "last_present": 540, "game_seconds_start": 3, "game_seconds_end": 11})
        final["sample_time_ns"] = initial["sample_time_ns"]+10000000000
        self.assertEqual(telemetry_rates(initial, final, 10)["simulation_ratio"], .8)
        with self.assertRaises(ValueError):
            telemetry_rates(initial, initial, 10)

    def test_source_timing_handles_batched_delivery_and_unrelated_epoch(self):
        epoch = 9000000000000000000
        timing = [(9, {}), (10, {"sample_time_ns": epoch, "game_seconds": 0, "presents": 0}),
                  (10, {"sample_time_ns": epoch+1000000000, "game_seconds": 1, "presents": 60}),
                  (10.1, {"sample_time_ns": epoch+1000000000, "game_seconds": 100, "presents": 60}),
                  (10.2, {"sample_time_ns": epoch+2000000000, "game_seconds": 2, "presents": 120}), (12, {})]
        summary = simulation_windows(timing, 10, 11)
        self.assertEqual({key: value for key, value in summary.items() if key != "windows"},
                         {"min_ratio": 1, "p05_ratio": 1, "max_ratio": 1, "below_0_95": 0, "count": 2})
        self.assertEqual(present_intervals(timing, 10, 11)["p99_ms"], 1000)

    def test_cap_baselines_require_applied_limit_and_new_source_sample(self):
        games = [SimpleNamespace(frame={"sample_time_ns": 100, "fps_limit": 240}, set_options=Mock()),
                 SimpleNamespace(frame={"sample_time_ns": 200, "fps_limit": 240}, set_options=Mock())]
        for game, cap in zip(games, (200, 30)):
            def wait(predicate, game=game, cap=cap, **kwargs):
                previous = game.frame["sample_time_ns"]
                self.assertFalse(predicate({"sample_time_ns": previous+1, "fps_limit": 240}))
                self.assertFalse(predicate({"sample_time_ns": previous, "fps_limit": cap}))
                game.frame = {"sample_time_ns": previous+2, "fps_limit": cap}
                self.assertTrue(predicate(game.frame))
            game.wait = wait
        host, client = applied_caps(*games, 200, 30)
        self.assertEqual((host["fps_limit"], client["fps_limit"]), (200, 30))
        self.assertEqual((host["sample_time_ns"], client["sample_time_ns"]), (102, 202))

    def test_performance_failures_accumulate_and_prevent_ok(self):
        report = {"ok": True}
        phases = (("baseline RTT 100 ms", .94, .95, 1.05),
                  ("stress spike RTT 100 ms", .9, .95, 1.05),
                  ("FPS ceilings 200/30", .97, .98, 1.02),
                  ("FPS ceilings 30/200", 1.03, .98, 1.02))
        with patch("builtins.print") as printed:
            for phase, ratio, lower, upper in phases:
                self.assertFalse(record_rates(report, phase, {"host": ratio, "client": 1}, lower, upper))
            self.assertTrue(record_rates(report, "later healthy phase", {"host": 1, "client": 1}))
        self.assertFalse(report["ok"])
        self.assertEqual([failure["phase"] for failure in report["performance_failures"]],
                         [phase for phase, *_ in phases])
        self.assertEqual(printed.call_count, 4)
        self.assertIn("PERFORMANCE FAILURE", printed.call_args_list[0].args[0])

    def test_simulation_windows_expose_slowdown_followed_by_catchup(self):
        timing = [(at, {"game_seconds": seconds}) for at, seconds in
                  ((0, 0), (.5, .25), (1, .5), (1.5, 1.25), (2, 2), (2.5, 2.5), (3, 3))]
        self.assertEqual((timing[-1][1]["game_seconds"]-timing[0][1]["game_seconds"])/3, 1)
        summary = simulation_windows(timing, 0, 3)
        self.assertEqual({key: value for key, value in summary.items() if key != "windows"},
                         {"min_ratio": .5, "p05_ratio": .5, "max_ratio": 1.5,
                          "below_0_95": 1, "count": 3})
        report = {"ok": True}
        with patch("builtins.print"):
            self.assertFalse(record_windows(report, "stress", "host", summary))
        self.assertFalse(report["ok"])
        self.assertEqual(len(report["performance_failures"][0]["failed_windows"]), 2)

    def test_simulation_windows_ignore_duplicate_backward_and_outside_timestamps(self):
        timing = [(-1, {}), (0, {"game_seconds": 0}), (.8, {"game_seconds": .8}),
                  (.8, {"game_seconds": 100}), (.7, {"game_seconds": 100}),
                  (1, {"game_seconds": 1}), (1.2, {"game_seconds": 1.2}), (3, {})]
        summary = simulation_windows(timing, 0, 2)
        self.assertEqual({key: value for key, value in summary.items() if key != "windows"},
                         {"min_ratio": 1, "p05_ratio": 1, "max_ratio": 1,
                          "below_0_95": 0, "count": 1})
        self.assertEqual(simulation_windows(timing, 4, 5)["count"], 0)

    def test_frontier_windows_preserve_rewinding_raw_clock_for_attribution(self):
        timing = [(at, {"sample_time_ns": 900000000000+int(at*1e9), "simulation_seconds": at,
                        "game_seconds": raw, "presents": int(at*120)})
                  for at, raw in ((0, 10), (1, 9.5), (2, 12))]
        summary = simulation_windows(timing, 0, 2)
        self.assertEqual((summary["min_ratio"], summary["max_ratio"]), (1, 1))
        window = summary["windows"][0]
        self.assertEqual((window["start_time_ns"], window["end_time_ns"]), (900000000000, 901000000000))
        self.assertEqual(window["game_seconds_end"]-window["game_seconds_start"], -.5)
        self.assertTrue(record_windows({}, "stress", "host", summary))
        self.assertEqual(telemetry_rates(timing[0][1], timing[1][1], 50)["simulation_ratio"], 1)

    def test_native_intervals_slice_completed_csv_and_gate_stalls_and_tail_latency(self):
        with tempfile.TemporaryDirectory() as folder:
            game = SimpleNamespace(run_dir=Path(folder))
            def measure(gaps, first=1, last=None):
                (game.run_dir/"frames.csv").write_text("t_ms,interval_ms\n" +
                    "".join(f"{index},{gap}\n" for index, gap in enumerate(gaps)))
                report = {"ok": True, "network_stress": {"profile": "spike", "performance_ok": True,
                    "host": {"first_present": first, "last_present": last or len(gaps)+1}}}
                with patch("builtins.print"):
                    native_stress_intervals(report, [game])
                return report
            report = measure([250]+[8]*100+[250], 2, 102)
            self.assertTrue(report["network_stress"]["host"]["native_present_intervals"]["ok"])
            report = measure([8]*100+[250])
            self.assertFalse(report["ok"])
            self.assertEqual(report["performance_failures"][0]["max_ms"], 250)
            self.assertLess(report["performance_failures"][0]["p99_ms"], 16.7)
            self.assertFalse(measure([8]*98+[20]*2)["ok"])
            self.assertFalse(measure([8], last=10)["ok"])
            self.assertFalse(measure([float("nan")])["ok"])

    def test_present_intervals_ignore_repeated_counters(self):
        timing = [(0, {"presents": 1}), (.005, {"presents": 1}),
                  (.010, {"presents": 2}), (.040, {"presents": 4}),
                  (.100, {"presents": 5})]
        self.assertEqual(present_intervals(timing, 0, .05),
                         {"p50_ms": 20.0, "p95_ms": 30.0, "p99_ms": 30.0, "max_ms": 30.0,
                          "samples": 2, "unobserved_presents": 1})

    def test_rollback_log_uses_confirmed_frame_and_latest_counters(self):
        hashes, counters = rollback_log(
            "[netplay] frame 120 hash ABCD\n"
            "[netplay] rollback count 1 replayed 4 max_depth 4 confirmed 120 head 124\n"
            "[netplay] frame 180 hash DEFA\n"
            "[netplay] rollback count 2 replayed 7 max_depth 4 confirmed 180 head 183\n")
        self.assertEqual(hashes, {120: "ABCD", 180: "DEFA"})
        self.assertEqual(counters, {"count": 2, "replayed": 7, "max_depth": 4,
                                    "confirmed": 180, "head": 183})

    def test_stress_drives_both_players_and_reports_matching_frames(self):
        with tempfile.TemporaryDirectory() as folder:
            games = []
            for name in ("host", "client"):
                directory = Path(folder)/name
                directory.mkdir()
                (directory/"run.log").write_text(
                    "[netplay] frame 1 hash AAAA\n"
                    "[netplay] rollback count 1 replayed 2 max_depth 2 confirmed 1 head 2\n")
                games.append(SimpleNamespace(run_dir=directory, _cond=threading.Condition(),
                    frame={"presents": 0, "game_seconds": 0, "context": "gameplay", "sample_time_ns": 500000000000},
                    timing=[], input_timing=[], _input_sent={}, aim=Mock(), key=Mock(), _input=Mock()))
            clock = [0.0]

            def wait(predicate, **kwargs):
                for game in games:
                    (game.run_dir/"run.log").write_text(
                        "[netplay] frame 100 hash BBBB\n"
                        "[netplay] rollback count 5 replayed 12 max_depth 3 confirmed 100 head 103\n")
                for tick in range(1, 121):
                    clock[0] = tick*.25
                    for index, game in enumerate(games):
                        if index == 1 and tick > 112:
                            continue # Client telemetry delivery stalls for the final two wall seconds.
                        game.frame = {**game.frame, "presents": tick*8, "game_seconds": clock[0],
                                      "sample_time_ns": 500000000000+int(clock[0]*1e9)}
                        game.timing.append((clock[0], dict(game.frame)))
                    if predicate(games[0].frame):
                        return
                self.fail("stress did not finish")

            games[0].wait = wait
            relay = Mock()
            relay.stats.return_value = {"udp_datagrams": 100}
            report = {}
            with patch("uctest.netplay.time.perf_counter", side_effect=lambda: clock[0]):
                unstable_network(*games, relay, report, "loss5", 100)
            result = report["network_stress"]
            self.assertTrue(result["completed"])
            self.assertFalse(result["performance_ok"])
            self.assertFalse(report["ok"])
            self.assertEqual(len(report["performance_failures"]), 1)
            self.assertEqual(report["performance_failures"][0]["kind"], "measurement")
            self.assertEqual(report["performance_failures"][0]["peer"], "client")
            self.assertEqual(result["client"]["sample_seconds"], 28)
            self.assertEqual(result["client"]["measurement"]["last_arrival_age_seconds"], 2)
            self.assertTrue(result["host"]["measurement"]["ok"])
            self.assertEqual(result["confirmed_hashes"]["matching"], 1)
            for name, game in zip(("host", "client"), games):
                self.assertEqual(result[name]["simulation_ratio"], 1)
                self.assertEqual(result[name]["simulation_windows"]["min_ratio"], 1)
                self.assertEqual(result[name]["simulation_windows"]["below_0_95"], 0)
                self.assertEqual(result[name]["rollback"]["count"], 4)
                self.assertEqual(result[name]["rollback"]["replayed"], 10)
                self.assertEqual(result[name]["rollback"]["max_depth"], 3)
                self.assertEqual(result[name]["present_intervals"]["p99_ms"], 250)
                self.assertGreater(game.aim.call_count, 50)
                self.assertGreater(game._input.call_count, 20)
                self.assertGreater(game.key.call_count, 5)
                self.assertTrue(all(call.kwargs["wait"] is False for call in game.aim.call_args_list))
                self.assertTrue(all(call.kwargs["wait"] is False for call in game._input.call_args_list))

    def test_input_ack_timing_keeps_fire_and_forget_consumption(self):
        game = Game(launch=False)
        game.transport = Mock()
        game.transport.read_line.side_effect = [json.dumps({"id": 1, "consumed_frame": 42}), None]
        with patch("uctest.client.time.perf_counter", side_effect=[1.0, 1.125]):
            game._input({"cmd": "input", "kpad": {"x": 0, "y": 0, "buttons": 0}}, wait=False)
            game._read_loop()
        self.assertEqual(list(game.input_timing), [(1.0, 1.125, True)])
        self.assertEqual(game._input_sent, {})
        self.assertEqual(game._replies, {})
        self.assertEqual(game._ignored, set())

    def test_input_latency_exposes_rejections_and_unconsumed_inputs(self):
        summary = input_consumption_latency([(0, .2, True), (1, 1.125, True), (1.5, 1.51, False)], [1.75, 0], 1, 2)
        self.assertEqual(summary, {"samples": 1, "p50_ms": 125, "p95_ms": 125, "max_ms": 125,
                                   "rejected": 1, "pending_at_end": 1, "oldest_pending_ms": 250})


if __name__ == "__main__":
    unittest.main()
