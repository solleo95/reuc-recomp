import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(sys.argv.pop(1))
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location("timing_audit", ROOT / "tools/timing_audit.py")
audit = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = audit
spec.loader.exec_module(audit)


def source(*instructions):
    lines = ["void fn_80200000(ppc::Ctx& c) {", "L_80200000:;"]
    for i, instruction in enumerate(instructions):
        lines.append("  /*%08X*/ { %s }" % (0x80200000 + 4 * i, instruction))
    return "\n".join(lines + ["}"])


def scan(*instructions):
    return audit.scan_source(source(*instructions), "fixture.cpp")[0]["findings"]


class TimingAuditTest(unittest.TestCase):
    def test_persistent_counter_with_pointer_alias(self):
        rows = scan("ppc::op_or(c, 3, 31, 3, false);",
                    "ppc::op_lwz(c, 4, 31, 7752);",
                    "ppc::op_addi(c, 4, 4, 1);",
                    "ppc::op_stw(c, 4, 3, 7752);")
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["kind"], "integer-step")
        self.assertEqual(rows[0]["address"], "8020000C")
        self.assertFalse(rows[0]["time_dependency"])

    def test_float_damping_and_integration(self):
        rows = scan("ppc::op_lfs(c, 0, 3, 12);",
                    "ppc::op_lfs(c, 1, 2, -100);",
                    "ppc::op_fmuls(c, 0, 0, 1, false);",
                    "ppc::op_stfs(c, 0, 3, 12);",
                    "ppc::op_lfs(c, 2, 3, 16);",
                    "ppc::op_fadds(c, 2, 2, 0, false);",
                    "ppc::op_stfs(c, 2, 3, 16);")
        self.assertEqual([r["kind"] for r in rows], ["float-multiply", "float-add"])

    def test_direct_clock_dependency_is_reported_not_declared_safe(self):
        rows = scan("ppc::op_addis(c, 5, 0, -32691);",
                    "ppc::op_addi(c, 5, 5, 13080);",
                    "ppc::op_lfs(c, 1, 5, 4);",
                    "ppc::op_lfs(c, 0, 3, 12);",
                    "ppc::op_fadds(c, 0, 0, 1, false);",
                    "ppc::op_stfs(c, 0, 3, 12);")
        self.assertTrue(rows[0]["time_dependency"])
        self.assertEqual(rows[0]["priority"], "review-time-use")

    def test_clock_in_unrelated_expression_does_not_clear_candidate(self):
        rows = scan("ppc::op_addis(c, 5, 0, -32691);",
                    "ppc::op_addi(c, 5, 5, 13080);",
                    "ppc::op_lfs(c, 1, 5, 4);",
                    "ppc::op_lfs(c, 0, 3, 12);",
                    "ppc::op_lfs(c, 2, 2, -100);",
                    "ppc::op_fadds(c, 0, 0, 2, false);",
                    "ppc::op_stfs(c, 0, 3, 12);")
        self.assertFalse(rows[0]["time_dependency"])

    def test_stack_initialization_and_register_only_loop_are_excluded(self):
        self.assertEqual(scan("ppc::op_lwz(c, 0, 1, 12);",
                              "ppc::op_addi(c, 0, 0, 1);",
                              "ppc::op_stw(c, 0, 1, 12);",
                              "ppc::op_addi(c, 0, 0, 3);",
                              "ppc::op_stw(c, 0, 3, 20);"), [])

    def test_addi_zero_base_is_literal_not_register_zero(self):
        self.assertEqual(scan("ppc::op_lwz(c, 0, 3, 12);",
                              "ppc::op_addi(c, 0, 0, 1);",
                              "ppc::op_stw(c, 0, 3, 12);"), [])

    def test_addic_countdown_and_real_register_zero(self):
        for operation in ("addic", "addic_"):
            rows = scan("ppc::op_lwz(c, 0, 3, 72);",
                        f"ppc::op_{operation}(c, 0, 0, -1);",
                        "ppc::op_stw(c, 0, 3, 72);")
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["kind"], "integer-step")

    def test_stack_pointer_alias_after_prologue_is_excluded(self):
        self.assertEqual(scan("ppc::op_stwu(c, 1, 1, -32);",
                              "ppc::op_addi(c, 3, 1, 12);",
                              "ppc::op_lfs(c, 0, 3, 0);",
                              "ppc::op_fadds(c, 0, 0, 1, false);",
                              "ppc::op_stfs(c, 0, 3, 0);"), [])

    def test_overwrite_and_branch_do_not_reuse_stale_dependencies(self):
        for barrier in ("ppc::op_fmr(c, 0, 3, false);",
                        "{ if (ppc::crbit(c, 2)) { goto L_80200000; } }",
                        "c.lr = 0x8020000Cu; fn_80001000(c);",
                        "ppc::op_unknown(c, 0);"):
            self.assertEqual(scan("ppc::op_lfs(c, 0, 3, 12);",
                                  "ppc::op_fadds(c, 0, 0, 1, false);",
                                  barrier, "ppc::op_stfs(c, 0, 3, 12);"), [])

    def test_same_register_different_pointer_is_not_same_field(self):
        self.assertEqual(scan("ppc::op_lfs(c, 0, 3, 12);",
                              "ppc::op_fadds(c, 0, 0, 1, false);",
                              "ppc::op_lwz(c, 3, 3, 0);",
                              "ppc::op_stfs(c, 0, 3, 12);"), [])

    def test_paired_single_updates_and_loop_context(self):
        rows = scan("ppc::op_psq_l(c, 0, 3, 12, 0, 0);",
                    "ppc::op_ps_add(c, 0, 0, 1, false);",
                    "ppc::op_psq_st(c, 0, 3, 12, 0, 0);",
                    "{ if (ppc::crbit(c, 2)) { goto L_80200000; } }")
        self.assertEqual(rows[0]["kind"], "float-add")
        self.assertTrue(rows[0]["in_loop"])

    def test_call_graph_hle_and_partial_hook_annotation(self):
        functions = audit.scan_source(source("c.lr = 0x80200004u; hle_FlightStep(c);",
                                              "fn_80001000(c); return;"), "fixture.cpp")
        self.assertEqual(functions[0]["callees"], ["80001000"])
        self.assertEqual(functions[0]["hle_calls"], ["FlightStep"])
        report = audit.make_report(functions, {"801C4438": "FlightStep"})
        self.assertIn("conditional", report["hooks"][0]["coverage"])
        self.assertIn("not proof", report["limitations"])

    def test_active_generation_manifest_and_empty_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            gen = base / "generation_active"
            gen.mkdir()
            (gen / "game_000.cpp").write_text(source("return;"), encoding="utf-8")
            (base / "game_sources.cmake").write_text(
                'set(UC_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/generation_active")', encoding="utf-8")
            self.assertEqual(audit.resolve_generation(base), gen)
            with self.assertRaises(ValueError):
                audit.scan_source("void unexpected_format() {}", "bad.cpp")

    def test_split_aot_body_owns_its_stores_and_alias_is_not_duplicated(self):
        text = source("return;") + "\n" + "\n".join([
            "static void body_80200100(ppc::Ctx& c, uint32_t at) {",
            "  /*80200100*/ { ppc::op_lfs(c, 0, 3, 12); }",
            "  /*80200104*/ { ppc::op_fadds(c, 0, 0, 1, false); }",
            "  /*80200108*/ { ppc::op_stfs(c, 0, 3, 12); }",
            "}",
            "void fn_80200100(ppc::Ctx& c) { body_80200100(c, 0x80200100); }",
        ])
        functions = audit.scan_source(text, "fixture.cpp")
        self.assertEqual([f["address"] for f in functions], ["80200000", "80200100"])
        self.assertEqual(functions[0]["findings"], [])
        self.assertEqual(functions[1]["findings"][0]["address"], "80200108")

    def test_cli_reports_and_generation_hook_provenance(self):
        with tempfile.TemporaryDirectory() as tmp:
            gen = Path(tmp) / "generation_fixture"
            gen.mkdir()
            (gen / "game_000.cpp").write_text(source(
                "ppc::op_lfs(c, 0, 3, 12);", "ppc::op_fadds(c, 0, 0, 1, false);",
                "ppc::op_stfs(c, 0, 3, 12);"), encoding="utf-8")
            (gen / "game_table.cpp").write_text("{0x801C4438u, hle_FlightStep},", encoding="utf-8")
            output = Path(tmp) / "report"
            run = subprocess.run([sys.executable, "-I", str(ROOT / "tools/timing_audit.py"),
                                  "--generation", str(gen), "--output", str(output),
                                  "--focus", "80200000:80200000"], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            report = json.loads((output / "report.json").read_text(encoding="utf-8"))
            self.assertEqual(report["summary"]["findings"], 1)
            self.assertEqual([h["name"] for h in report["hooks"]], ["FlightStep"])
            self.assertIn("80200008", (output / "focus.md").read_text(encoding="utf-8"))
            self.assertIn("source_sha256", report["provenance"])

    def test_baseline_ledger_retains_unique_candidates_and_evidence(self):
        ledger = json.loads((ROOT / "docs/testing/timing-triage.json").read_text(encoding="utf-8"))
        rows = ledger["findings"]
        self.assertEqual(len(rows), 3513)
        self.assertEqual(len({row["store"] for row in rows}), 3513)
        counts = {}
        for row in rows:
            self.assertRegex(row["store"], r"^[0-9A-F]{8}$")
            self.assertTrue(row["reason"] and row["evidence"], row["store"])
            self.assertIn(row["status"], ("fixed", "already-timed", "not-frame-based", "unresolved"))
            counts[row["status"]] = counts.get(row["status"], 0) + 1
        self.assertEqual(ledger["counts"], counts)
        if ledger["complete"]:
            self.assertNotIn("unresolved", counts)


if __name__ == "__main__":
    unittest.main()
