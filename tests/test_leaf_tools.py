import sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(sys.argv[1]) / "tools"))
import leaf_hle, leaf_hybrid

BODY_COND_RETURN = """  UC_ENTER(0x80235294u);
  /*80235294*/ { ppc::op_lwz(c, 0, 3, 8); }
  /*80235298*/ { ppc::op_cmpi(c, 0, 0, 0); }
  /*8023529C*/ { { if (ppc::crbit(c, 2) == true) { return; } } }
  /*802352A0*/ { ppc::op_addi(c, 3, 3, 4); }
  /*802352A4*/ { return; }
"""
BODY_BDNZ = """  UC_ENTER(0x802647ACu);
  /*802647AC*/ { ppc::op_addi(c, 5, 0, 4); }
  /*802647B0*/ { ppc::op_mtspr(c, 5, 9); }
L_802647B4:;
  /*802647B4*/ { ppc::op_lwz(c, 6, 3, 0); }
  /*802647B8*/ { ppc::op_stw(c, 6, 4, 0); }
  /*802647BC*/ { { --c.ctr; if (c.ctr != 0) { ppc::poll(c); goto L_802647B4; } } }
  /*802647C0*/ { return; }
"""
BODY_NMSUB_STMW = """  UC_ENTER(0x80369E68u);
  /*80369E68*/ { ppc::op_stmw(c, 28, 1, -16); }
  /*80369E6C*/ { ppc::op_ps_nmsub(c, 1, 2, 3, 4, false); }
  /*80369E70*/ { ppc::op_lmw(c, 28, 1, -16); }
  /*80369E74*/ { return; }
"""
BODY_CALL = """  /*80000000*/ { c.lr = 0x80000004u; fn_80000100(c); }
"""


def items_of(body):
    parsed, why = leaf_hybrid.parse_hybrid(body)
    return parsed[0] if parsed else why


class HybridParseTest(unittest.TestCase):
    def test_conditional_return_is_a_branch_to_the_end(self):
        items = items_of(BODY_COND_RETURN)
        self.assertIn(("branch", "ppc::crbit(c, 2) == true", "L_done", False), items)

    def test_bdnz_is_a_counter_branch_with_its_poll(self):
        items = items_of(BODY_BDNZ)
        self.assertIn(("bdnz", "L_802647B4", True), items)
        self.assertTrue(leaf_hybrid.has_loop(items))
        self.assertTrue(leaf_hybrid.analyse(items)["ctr_used"])

    def test_nmsub_stmw_lmw_are_accepted_and_use_the_register_range(self):
        items = items_of(BODY_NMSUB_STMW)
        self.assertIsInstance(items, list, items)
        info = leaf_hybrid.analyse(items)
        self.assertTrue({28, 29, 30, 31} <= info["ints"])
        self.assertTrue({28, 29, 30, 31} <= info["ints_w"])   # lmw writes them (the epilogue restores the caller's values)
        self.assertTrue({1, 2, 3, 4} <= info["fl"])

    def test_a_call_is_still_rejected(self):
        self.assertEqual(items_of(BODY_CALL), "calls another function")


class LoadTest(unittest.TestCase):
    def test_a_function_with_extra_entry_points_is_rejected_with_a_reason(self):
        import tempfile
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / "game_001.cpp").write_text("static void body_80000010(ppc::Ctx& c, uint32_t at) {\n  UC_ENTER(0x80000010u);\n}\nvoid fn_80000010(ppc::Ctx& c) { body_80000010(c, 0); }\nvoid fn_80000020(ppc::Ctx& c) {\n  /*80000020*/ { return; }\n}\n")
            self.assertEqual(leaf_hle.load_function(Path(folder), "80000010"), (None, "extra entry points (a body_ function)"))
            self.assertEqual(leaf_hle.load_function(Path(folder), "80000020")[1], None)
            self.assertEqual(leaf_hle.load_function(Path(folder), "80000099"), (None, "not found in the reference generation"))


class SpliceTest(unittest.TestCase):
    CODE_OLD = "// header\nnamespace {\nvoid slow_Leaf_80000010(ppc::Ctx& c) {\n}\n}  // namespace\nvoid hle_Leaf_80000010(ppc::Ctx& c) {\n}\n"
    TEST_OLD = ('#include x\nvoid hle_Leaf_80000010(Ctx&);\n\nnamespace {\nvoid ref_Leaf_80000010(Ctx& c) {\n}\nconstexpr uint32_t kBase = 1;\n}\n'
                'int main() {\n    compare("Leaf_80000010", hle_Leaf_80000010, ref_Leaf_80000010, {}, false);\n    return 0;\n}\n')
    CODE_NEW = "// header\nnamespace {\nusing ppc::Ctx;\n}\nnamespace {\nvoid slow_Leaf_80000020(ppc::Ctx& c) {\n}\n}  // namespace\nvoid hle_Leaf_80000020(ppc::Ctx& c) {\n}\n"
    TEST_NEW = ('#include x\nvoid hle_Leaf_80000020(Ctx&);\n\nnamespace {\nvoid ref_Leaf_80000020(Ctx& c) {\n}\nconstexpr uint32_t kBase = 1;\n}\n'
                'int main() {\n    compare("Leaf_80000020", hle_Leaf_80000020, ref_Leaf_80000020, {4}, true);\n    return 0;\n}\n')

    def test_new_functions_are_added_and_the_old_text_is_untouched(self):
        code, test = leaf_hle.splice(self.CODE_OLD, self.TEST_OLD, self.CODE_NEW, self.TEST_NEW)
        self.assertTrue(code.startswith(self.CODE_OLD.rstrip("\n")))
        self.assertIn("void hle_Leaf_80000020(", code)
        self.assertEqual(code.count("using ppc::Ctx;"), 0)   # the new file's header is not repeated
        for piece in ("void hle_Leaf_80000010(Ctx&);\nvoid hle_Leaf_80000020(Ctx&);", "void ref_Leaf_80000020(Ctx& c) {", 'compare("Leaf_80000010"', 'compare("Leaf_80000020"'):
            self.assertIn(piece, test)
        self.assertLess(test.index("void ref_Leaf_80000020"), test.index("constexpr uint32_t kBase"))
        self.assertLess(test.index('compare("Leaf_80000020"'), test.rindex("return 0;"))
        self.assertEqual(test.count('compare("Leaf_80000010"'), 1)


class RegenerationNoticesTest(unittest.TestCase):
    def test_requested_functions_that_already_exist_are_reported_as_skipped(self):
        generated = "void hle_Leaf_80137BC4(ppc::Ctx& c) {\n}\n"
        self.assertEqual(leaf_hle.skipped_addresses(generated, ["80137bc4", "802647AC"]), ["80137BC4"])

    def test_a_changed_generator_header_is_noticed(self):
        old = "// header v1\nnamespace {\nusing x;\n}\nnamespace {\nvoid slow_Leaf_80000010(ppc::Ctx& c) {\n}\n"
        self.assertFalse(leaf_hle.header_differs(old, old.replace("80000010", "80000020")))
        self.assertTrue(leaf_hle.header_differs(old, old.replace("v1", "v2")))


class NewAddressesTest(unittest.TestCase):
    def test_only_functions_missing_from_the_generated_file_are_new(self):
        generated = "void hle_Leaf_80064C34(ppc::Ctx& c) {\n}\nvoid hle_Leaf_80137BC4(ppc::Ctx& c) {\n}\n"
        self.assertEqual(leaf_hle.new_addresses(generated, ["802647ac", "80137BC4", "802647AC"]), ["802647AC"])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
