import sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(sys.argv[1]) / "scripts"))
import inclusive_profile as ip

NAMES = {0x10: "A", 0x20: "B", 0x30: "C"}


class InclusiveTest(unittest.TestCase):
    def test_counts_each_function_once_per_sample(self):
        # stack A <- B <- C: the instruction pointer is in A, called by B, called by C
        samples = [[0x10, 0x20, 0x30]] * 3 + [[0x10, 0x20]]
        incl, own, total = ip.inclusive(samples, NAMES.get)
        self.assertEqual(total, 4)
        self.assertEqual((incl["A"], incl["B"], incl["C"]), (4, 4, 3))
        self.assertEqual(own["A"], 4)
        self.assertNotIn("B", own)

    def test_recursion_counts_the_function_once(self):
        incl, own, total = ip.inclusive([[0x10, 0x10, 0x20]], NAMES.get)
        self.assertEqual((incl["A"], incl["B"], own["A"], total), (1, 1, 1, 1))

    def test_unknown_addresses_are_ignored(self):
        incl, own, total = ip.inclusive([[0x99, 0x20]], NAMES.get)   # the instruction pointer is in a library: B still gets its time, nothing is "self"
        self.assertEqual((incl["B"], dict(own), total), (1, {}, 1))

    def test_symbolizer_output_is_matched_by_address_not_by_position(self):
        output = "0x10\nfuncA\nfile.cpp:1:2\n\n0x20\n??\n??:0:0\n\n0x30\nfuncC\nextra line\nfile.cpp:3:4\n\n"
        self.assertEqual(ip.parse_symbolizer(output), {0x10: "funcA", 0x20: None, 0x30: "funcC"})

    def test_the_symbolizer_comes_from_the_environment_then_the_path_then_the_default(self):
        self.assertEqual(ip.find_symbolizer({"UC_LLVM_SYMBOLIZER": "X:/s.exe"}, lambda n: "P:/llvm-symbolizer"), "X:/s.exe")
        self.assertEqual(ip.find_symbolizer({}, lambda n: "P:/llvm-symbolizer"), "P:/llvm-symbolizer")
        self.assertEqual(ip.find_symbolizer({}, lambda n: None), ip.DEFAULT_SYMBOLIZER)

    def test_samples_without_a_stack_are_counted(self):
        self.assertEqual(ip.unwound_fraction([[0x10, 0x20], [0x10], [0x10, 0x20, 0x30], []]), 0.5)   # a sample with only its own address (taken inside a library) has no stack
        self.assertEqual(ip.unwound_fraction([]), 0.0)

    def test_parse_reads_base_and_stack_lines_only(self):
        text = "# base 1000\n5 2000 7\nx 5 memcpy 8\ns 5 9 1010 1020 1030\ns 5 10 1010\nw exe 1 2\n"
        base, samples = ip.parse(text)
        self.assertEqual(base, 0x1000)
        self.assertEqual(samples, [(5, [0x10, 0x20, 0x30]), (5, [0x10])])   # addresses relative to the image base


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
