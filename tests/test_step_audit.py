import os
import sys
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(sys.argv.pop(1))
sys.path.insert(0, str(ROOT / "tools"))
from step_audit import ir  # noqa: E402

FULL = os.environ.get("UC_STEP_AUDIT_FULL") == "1"


def shard(*functions):
    """functions: (address, [instruction text, ...]); labels are written as 'L:' entries."""
    lines = []
    for address, body in functions:
        lines += [f"void fn_{address:08X}(ppc::Ctx& c) {{", f"  UC_ENTER(0x{address:08X}u);", f"L_{address:08X}:;"]
        pc = address
        for text in body:
            if text == "L:":
                lines.append(f"L_{pc:08X}:;")
                continue
            lines.append("  /*%08X*/ { %s }" % (pc, text))
            pc += 4
        lines.append("}")
    return "\n".join(lines)


class IrTest(unittest.TestCase):
    def test_parses_calls_and_slots(self):
        functions = ir.parse_shard(shard((0x80100000, [
            "c.lr = 0x80100004u; fn_80200000(c);",
            "c.lr = 0x80100008u; hle_Foo(c);",
            "ppc::op_lwz(c, 12, 3, 0);",
            "ppc::op_lwz(c, 12, 12, 12);",
            "ppc::op_mtspr(c, 12, 9);",
            "c.lr = 0x80100018u; uc::call_indirect(c, c.ctr);",
            "return;",
        ])), "x.cpp")
        insns = functions["80100000"].insns
        self.assertEqual(insns[0].call, "80200000")
        self.assertEqual(insns[1].call, "hle:Foo")
        self.assertEqual(insns[5].indirect_slot, 12)
        self.assertTrue(insns[6].returns)
        self.assertEqual(insns[2].op, "lwz")
        self.assertEqual(insns[2].args, (12, 3, 0))
        self.assertEqual(insns[2].roles, ("Id", "Ib", "N"))

    def test_prologue_frame(self):
        functions = ir.parse_shard(shard((0x80100000, ["ppc::op_stwu(c, 1, 1, -256);", "return;"]),
                                         (0x80100100, ["ppc::op_lwz(c, 3, 3, 0);", "return;"])), "x.cpp")
        self.assertEqual(functions["80100000"].prologue_frame, 256)
        self.assertIsNone(functions["80100100"].prologue_frame)

    def test_branch_targets_and_labels(self):
        functions = ir.parse_shard(shard((0x80100000, [
            "ppc::op_cmpi(c, 0, 3, 0);",
            "{ if (ppc::crbit(c, 2) == true) { goto L_8010000C; } }",
            "ppc::op_addi(c, 3, 0, 1);",
            "L:",
            "return;",
        ])), "x.cpp")
        f = functions["80100000"]
        self.assertEqual(f.insns[1].branch, (0x8010000C,))
        self.assertIn(0x8010000C, f.labels)

    def test_unparsed_instruction_is_marked(self):
        f = ir.parse_shard(shard((0x80100000, ["ppc::op_weird(c);", "return;"])), "x.cpp")["80100000"]
        self.assertEqual(f.insns[0].op, "?")

    @unittest.skipUnless(FULL, "set UC_STEP_AUDIT_FULL=1")
    def test_full_generation_loads(self):
        functions = ir.load_generation(ROOT / "generated")
        self.assertGreaterEqual(len(functions), 15000)
        self.assertEqual(functions["801696B8"].prologue_frame, 160)
        self.assertIn("80165614", functions)   # retained body of the hooked crossfade entry


def words(base, values):
    import struct
    return struct.pack(">%dI" % len(values), *values)


class ClassesTest(unittest.TestCase):
    def setUp(self):
        from step_audit import classes
        self.classes = classes
        # Two vtables at 0x80001000 and 0x80001040, objects pointing at them, junk elsewhere.
        mem = bytearray(0x4000)
        def put(addr, values):
            mem[addr - 0x80000000: addr - 0x80000000 + 4 * len(values)] = words(addr, values)
        put(0x80001000, [0, 0, 0x80100000, 0x80100010, 0x80100020])
        put(0x80001040, [0, 0, 0x80100030, 0x80100040, 0x80100020])
        put(0x80002000, [0x80001000, 7, 8])          # object A (vptr at +0)
        put(0x80002100, [1, 2, 3, 4, 0x80001040])    # object B (vptr at +16)
        put(0x80003000, [0x80100000, 0x12345678])    # a lone code pointer: not a vtable
        self.functions = {"80100000", "80100010", "80100020", "80100030", "80100040"}
        self.index = classes.scan_buffer(bytes(mem), 0x80000000, self.functions)

    def test_finds_vtable_and_objects(self):
        self.assertEqual(set(self.index.vtables), {0x80001000, 0x80001040})
        self.assertEqual(self.index.vtables[0x80001000][2], "80100000")
        self.assertEqual(self.index.objects[0x80001000], [0x80002000])
        self.assertEqual(self.index.objects[0x80001040], [0x80002110])

    def test_slot_targets_union(self):
        self.assertEqual(self.index.slot_targets(8), {"80100000", "80100030"})
        self.assertEqual(self.index.slot_targets(16), {"80100020"})
        merged = self.classes.merge([self.index, self.index])
        self.assertEqual(merged.slot_targets(12), {"80100010", "80100040"})

    @unittest.skipUnless(FULL, "set UC_STEP_AUDIT_FULL=1")
    def test_real_dump_vtables(self):
        dumps = sorted((ROOT / "build/autotest").glob("episode0-480--2-*/dump_1.*.bin"))
        self.assertTrue(dumps, "no chapter-0 dump available")
        functions = set(ir.load_generation(ROOT / "generated")) | self.classes.hle_entries(ROOT / "project/hle_map.txt")
        index = self.classes.scan_dump(dumps[-1], functions)
        self.assertEqual(index.vtables[0x803FA8EC][3], "80165610")
        self.assertEqual(index.vtables[0x804094F4][772 // 4], "800C790C")


CLOCK = ["ppc::op_addis(c, 4, 0, -32691);", "ppc::op_addi(c, 4, 4, 13080);"]


class KdimTest(unittest.TestCase):
    def setUp(self):
        from step_audit import kdim
        self.kdim = kdim

    def analyze(self, body, summaries=None):
        f = ir.parse_shard(shard((0x80100000, body)), "x.cpp")["80100000"]
        env = self.kdim.Env(summary=lambda callee, args: (summaries or {}).get(callee, {}))
        return self.kdim.analyze(f, env)

    def store(self, result, offset=16):
        found = [s for s in result.stores if s.key == (("entry", 31), offset)]
        self.assertEqual(len(found), 1, result.stores)
        return found[0]

    def test_dt_times_frames_is_k2(self):
        K = self.kdim.K
        r = self.analyze(["c.lr = 0x80100004u; fn_80300000(c);", *CLOCK, "ppc::op_lfs(c, 2, 4, 4);",
                          "ppc::op_fmuls(c, 3, 1, 2, false);", "ppc::op_lfs(c, 4, 31, 16);",
                          "ppc::op_fadds(c, 4, 4, 3, false);", "ppc::op_stfs(c, 4, 31, 16);", "return;"],
                         {"80300000": {"f1": self.kdim.exact(1)}})
        s = self.store(r)
        self.assertTrue(s.prior_load_same_key)
        self.assertEqual(s.delta_k, 2)

    def test_per_call_increment_is_k0(self):
        r = self.analyze(["ppc::op_lfs(c, 0, 31, 16);", "ppc::op_lfs(c, 1, 2, -15116);",
                          "ppc::op_fadds(c, 0, 0, 1, false);", "ppc::op_stfs(c, 0, 31, 16);", "return;"])
        s = self.store(r)
        self.assertTrue(s.prior_load_same_key)
        self.assertEqual(s.delta_k, 0)

    def test_dt_increment_is_k1(self):
        r = self.analyze([*CLOCK, "ppc::op_lfs(c, 1, 4, 4);", "ppc::op_lfs(c, 0, 31, 16);",
                          "ppc::op_fadds(c, 0, 0, 1, false);", "ppc::op_stfs(c, 0, 31, 16);", "return;"])
        self.assertEqual(self.store(r).delta_k, 1)

    def test_branch_merge_mixed_is_bottom(self):
        r = self.analyze(["ppc::op_cmpi(c, 0, 3, 0);",
                          "{ if (ppc::crbit(c, 2) == true) { goto L_80100018; } }",
                          *CLOCK, "ppc::op_lfs(c, 1, 4, 4);", "{ goto L_8010001C; }",
                          "L:", "ppc::op_lfs(c, 1, 2, -15116);",
                          "L:", "ppc::op_lfs(c, 0, 31, 16);", "ppc::op_fadds(c, 0, 0, 1, false);",
                          "ppc::op_stfs(c, 0, 31, 16);", "return;"])
        self.assertIsNone(self.store(r).delta_k)

    def test_fmadds_operand_order(self):
        # fmadds(t, a, b, c) computes a*c + b: dt * const + field is a k=1 increment of the field.
        r = self.analyze([*CLOCK, "ppc::op_lfs(c, 1, 4, 4);", "ppc::op_lfs(c, 3, 2, -15116);",
                          "ppc::op_lfs(c, 2, 31, 16);", "ppc::op_fmadds(c, 0, 1, 2, 3, false);",
                          "ppc::op_stfs(c, 0, 31, 16);", "return;"])
        self.assertEqual(self.store(r).delta_k, 1)

    def test_unknown_factor_keeps_known_part(self):
        # field(+40, unknown) * dt * dt added to +16: exact k unknown, known part 2.
        r = self.analyze([*CLOCK, "ppc::op_lfs(c, 1, 4, 4);", "ppc::op_lfs(c, 2, 31, 40);",
                          "ppc::op_fmuls(c, 2, 2, 1, false);", "ppc::op_fmuls(c, 2, 2, 1, false);",
                          "ppc::op_lfs(c, 0, 31, 16);", "ppc::op_fadds(c, 0, 0, 2, false);",
                          "ppc::op_stfs(c, 0, 31, 16);", "return;"])
        s = self.store(r)
        self.assertIsNone(s.delta_k)
        self.assertEqual((s.delta_kn, s.delta_unk), (2, True))

    def test_clock_rate_is_k0_and_return_value(self):
        r = self.analyze([*CLOCK, "ppc::op_lfs(c, 1, 4, 28);", "return;"])
        self.assertEqual(r.ret_k["f1"].k, 0)
        r = self.analyze([*CLOCK, "ppc::op_lfs(c, 1, 4, 4);", "return;"])
        self.assertEqual(r.ret_k["f1"].k, 1)


VT = 0x80001000
CLASS_FUNCS = (0x80100000, 0x80100100, 0x80100200)


def program_for(bodies, extra=None, empirical=None, body_hooks=None):
    from step_audit import classes, program
    funcs = {}
    for address, body in zip(CLASS_FUNCS, bodies):
        funcs.update(ir.parse_shard(shard((address, body)), "x.cpp"))
    for address, body in (extra or {}).items():
        funcs.update(ir.parse_shard(shard((address, body)), "x.cpp"))
    index = classes.DumpIndex({VT: [None, None] + [f"{a:08X}" for a in CLASS_FUNCS]}, {VT: [0x80002000]})
    return program.run_program(funcs, index, {}, empirical=empirical or {}, body_hooks=body_hooks or {})


STORE_DT = [*CLOCK, "ppc::op_lfs(c, 1, 4, 4);", "ppc::op_stfs(c, 1, 3, 20);", "return;"]
STORE_CONST = ["ppc::op_lfs(c, 1, 2, -15116);", "ppc::op_stfs(c, 1, 3, 20);", "return;"]
ACCUMULATE = [*CLOCK, "ppc::op_lfs(c, 1, 4, 4);", "ppc::op_lfs(c, 2, 3, 20);", "ppc::op_fmuls(c, 2, 2, 1, false);",
              "ppc::op_lfs(c, 0, 3, 24);", "ppc::op_fadds(c, 0, 0, 2, false);", "ppc::op_stfs(c, 0, 3, 24);", "return;"]


class ProgramTest(unittest.TestCase):
    def findings(self, prog):
        from step_audit import patterns
        return patterns.find_patterns(prog)

    def test_static_field_links_functions(self):
        prog = program_for([STORE_DT, ["return;"], ACCUMULATE])
        self.assertEqual(prog.field_k[(("class", VT), 20)], 1)
        p2 = [f for f in self.findings(prog) if f.pattern == "P2" and f.confidence == "high"]
        self.assertEqual([(f.function, f.store) for f in p2], [("80100200", 0x8010021C)])

    def test_mixed_field_is_bottom(self):
        prog = program_for([STORE_DT, STORE_CONST, ACCUMULATE])
        self.assertIsNone(prog.field_k.get((("class", VT), 20)))
        self.assertFalse([f for f in self.findings(prog) if f.pattern == "P2" and f.confidence == "high"])

    def test_empirical_type_used_when_static_unknown(self):
        prog = program_for([["return;"], ["return;"], ACCUMULATE], empirical={(("class", VT), 20): 1})
        self.assertTrue([f for f in self.findings(prog) if f.pattern == "P2" and f.confidence == "high"])

    def test_covered_site_reports_hook_name(self):
        prog = program_for([STORE_DT, ["return;"], ACCUMULATE], body_hooks={"80100200": "CrossfadeStep"})
        p2 = [f for f in self.findings(prog) if f.pattern == "P2"]
        self.assertEqual({f.covered_by for f in p2}, {"CrossfadeStep"})

    def test_non_bug_selection_index_not_flagged(self):
        gated = ["ppc::op_lwz(c, 0, 3, 8);", "ppc::op_rlwinm(c, 0, 0, 0, 20, 20, true);",
                 "{ if (ppc::crbit(c, 2) == true) { goto L_80100014; } }",
                 "ppc::op_lwz(c, 4, 3, 16);", "ppc::op_addi(c, 4, 4, 1);", "L:", "ppc::op_stw(c, 4, 3, 16);", "return;"]
        plain = ["ppc::op_lwz(c, 4, 3, 16);", "ppc::op_addi(c, 4, 4, -1);", "ppc::op_stw(c, 4, 3, 16);", "return;"]
        self.assertFalse([f for f in self.findings(program_for([gated, ["return;"], ["return;"]])) if f.pattern == "P5"])
        self.assertTrue([f for f in self.findings(program_for([plain, ["return;"], ["return;"]])) if f.pattern == "P5"])


CALLER = ["ppc::op_or(c, 3, 31, 3, false);", *CLOCK, "ppc::op_lfs(c, 1, 4, 4);",
          "c.lr = 0x80100314u; fn_80100400(c);", "return;"]
CALLEE = ["ppc::op_stwu(c, 1, 1, -64);", "ppc::op_fmuls(c, 2, 1, 1, false);",
          "ppc::op_lfs(c, 0, 3, 24);", "ppc::op_fadds(c, 0, 0, 2, false);",
          "ppc::op_stfs(c, 0, 3, 24);", "ppc::op_addi(c, 1, 1, 64);", "return;"]


class ReceiverTest(unittest.TestCase):
    def test_virtual_call_uses_receiver_class(self):
        from step_audit import classes, program, patterns
        W, X = 0x80001100, 0x80001200
        getter_dt = [*CLOCK, "ppc::op_lfs(c, 1, 4, 4);", "return;"]          # class W slot 8: returns dt (k=1)
        getter_one = ["ppc::op_lfs(c, 1, 2, -15116);", "return;"]           # class X slot 8: constant
        caller = ["ppc::op_or(c, 3, 31, 3, false);", "ppc::op_lwz(c, 3, 3, 8);", "ppc::op_lwz(c, 12, 3, 0);",
                  "ppc::op_lwz(c, 12, 12, 8);", "ppc::op_mtspr(c, 12, 9);", "c.lr = 0x80100518u; uc::call_indirect(c, c.ctr);",
                  "ppc::op_fmuls(c, 2, 1, 1, false);", "ppc::op_lfs(c, 0, 31, 24);", "ppc::op_fadds(c, 0, 0, 2, false);",
                  "ppc::op_stfs(c, 0, 31, 24);", "return;"]
        funcs = {}
        for a, body in ((0x80100500, caller), (0x80100600, getter_dt), (0x80100700, getter_one)):
            funcs.update(ir.parse_shard(shard((a, body)), "x.cpp"))
        index = classes.DumpIndex({VT: [None, None, "80100500"], W: [None, None, "80100600"], X: [None, None, "80100700"]},
                                  {VT: [0x80002000], W: [0x80002100], X: [0x80002200]})
        def p2(pointer_types):
            prog = program.run_program(funcs, index, {}, pointer_types=pointer_types)
            return [f for f in patterns.find_patterns(prog) if f.pattern == "P2" and f.confidence == "high"]
        self.assertFalse(p2({}))                                           # slot union mixes k=1 and k=0: unknown
        self.assertTrue(p2({(("class", VT), 8): W}))                       # receiver typed: dt * dt


class FixesTest(unittest.TestCase):
    def setUp(self):
        from step_audit import fixes, patterns
        self.fixes, self.patterns = fixes, patterns

    def records(self, callee=CALLEE, body_hooks=None):
        prog = program_for([["return;"], ["return;"], ["return;"]],
                           extra={0x80100300: CALLER, 0x80100400: callee}, body_hooks=body_hooks)
        findings = self.patterns.find_patterns(prog)
        return self.fixes.propose(findings, prog)

    def test_arg_double_step_becomes_scale_record(self):
        records, rejected = self.records()
        self.assertEqual([(r.entry, r.frame, r.lr, r.template, r.operand, r.power) for r in records],
                         [("80100400", 64, "80100314", "scale", "f1", -1)])

    def test_no_prologue_is_report_only(self):
        records, rejected = self.records(callee=CALLEE[1:-2] + ["return;"])
        self.assertEqual(records, [])
        self.assertTrue(any("prologue" in r.reason for r in rejected))

    def test_covered_site_gets_no_record(self):
        records, rejected = self.records(body_hooks={"80100400": "CrossfadeStep"})
        self.assertEqual(records, [])

    def test_sync_bindings_touches_only_block(self):
        import tempfile
        rec = self.fixes.FixRecord("80100400", 64, "80100310", "scale", "f1", -1, "test")
        with tempfile.TemporaryDirectory() as d:
            hle, cfg, inc = Path(d, "hle_map.txt"), Path(d, "uc.cfg"), Path(d, "pacing_fixes.inc")
            hle.write_text("@80000000 Keep\n", encoding="utf-8")
            cfg.write_text("entry 80000004\n", encoding="utf-8")
            for _ in range(2):   # idempotent
                self.fixes.sync_bindings([rec], hle, cfg)
                self.fixes.write_inc([rec], inc)
            self.assertEqual(hle.read_text(encoding="utf-8"),
                             "@80000000 Keep\n# BEGIN step_audit\n@80100400 PacingFix_0x80100400\n# END step_audit\n")
            self.assertEqual(cfg.read_text(encoding="utf-8"), "entry 80000004\n# BEGIN step_audit\nentry 80100404\n# END step_audit\n")
            text = inc.read_text(encoding="utf-8")
            self.assertIn("PACING_ENTRY(0x80100400, 64)", text)
            self.assertIn("PACING_FIX(0x80100400, 0x80100310, scale, 1, -1)", text)


class ProbeTest(unittest.TestCase):
    def test_prove_needs_context(self):
        from step_audit import probe
        f = ir.parse_shard(shard((0x80100400, ["ppc::op_stwu(c, 1, 1, -64);", "ppc::op_lfs(c, 0, 4, 8);", "return;"])), "x.cpp")["80100400"]
        self.assertEqual(probe.extra_inputs(f, "f1"), ["r4"])
        g = ir.parse_shard(shard((0x80100400, ["ppc::op_stwu(c, 1, 1, -64);", "ppc::op_addi(c, 4, 0, 1);",
                                               "ppc::op_lfs(c, 0, 4, 8);", "ppc::op_fmuls(c, 0, 0, 1, false);", "return;"])), "x.cpp")["80100400"]
        self.assertEqual(probe.extra_inputs(g, "f1"), [])

    def test_parse_probe_output(self):
        from step_audit import probe
        ok = probe.run_probe([sys.executable, "-c", "print('{\"status\":\"pass\",\"reason\":\"6 cases exact\",\"max_rel\":1e-07}')"])
        self.assertEqual((ok.status, ok.max_rel), ("pass", 1e-07))
        trap = probe.run_probe([sys.executable, "-c", "import sys; sys.stderr.write('guest trap at 80000000'); sys.exit(3)"])
        self.assertEqual(trap.status, "unproven")
        self.assertIn("guest trap", trap.reason)


class EmpiricalTest(unittest.TestCase):
    def test_ratio_types_fields(self):
        import struct
        from step_audit import classes, empirical
        def image(speed, rate, noise):
            mem = bytearray(0x4000)
            for obj, n in ((0x80002000, noise), (0x80002400, noise * 3)):
                struct.pack_into(">I", mem, obj - 0x80000000, VT)
                struct.pack_into(">fff", mem, obj + 16 - 0x80000000, speed, rate, n)
            return bytes(mem)
        index = classes.DumpIndex({VT: [None, None, "80100000"]}, {VT: [0x80002000, 0x80002400]})
        typed = empirical.type_fields([(image(1.0, 3.0, 0.7), image(0.0624, 3.0, 5.1))], 0x80000000, index, {VT: 0}, s=0.0624)
        self.assertEqual(typed[(("class", VT), 16)], 1)
        self.assertEqual(typed[(("class", VT), 20)], 0)
        self.assertNotIn((("class", VT), 24), typed)

    def test_pointer_fields_typed(self):
        import struct
        from step_audit import classes, empirical
        W = 0x80001100
        mem = bytearray(0x4000)
        for obj, target in ((0x80002000, 0x80003000), (0x80002400, 0x80003100)):
            struct.pack_into(">I", mem, obj - 0x80000000, VT)
            struct.pack_into(">I", mem, obj + 8 - 0x80000000, target)                  # +8 -> object of class W
            struct.pack_into(">I", mem, obj + 12 - 0x80000000, 0x80003000 if obj == 0x80002000 else 0x80002000)  # mixed
        for w in (0x80003000, 0x80003100):
            struct.pack_into(">I", mem, w + 16 - 0x80000000, W)                        # W's vptr at +16
        index = classes.DumpIndex({VT: [None, None, "80100000"], W: [None, None, "80100100"]},
                                  {VT: [0x80002000, 0x80002400], W: [0x80003010, 0x80003110]})
        types = empirical.pointer_types([bytes(mem)], 0x80000000, index, {VT: 0, W: 16})
        self.assertEqual(types[(("class", VT), 8)], W)
        self.assertNotIn((("class", VT), 12), types)


if __name__ == "__main__":
    unittest.main()
