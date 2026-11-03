import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import slangc_quiet  # noqa: E402

CHECKED = "fxc 47.0: D:/p/renderer/shaders/tev.slang(165): warning X4000:  use of potentially uninitialized variable (colorSource_0)\n"


class SlangcQuietTests(unittest.TestCase):
    def test_checked_warning_is_dropped(self):
        self.assertEqual(slangc_quiet.filtered(CHECKED), "")

    def test_same_code_in_another_shader_and_other_codes_are_kept(self):
        other_file = CHECKED.replace("tev.slang", "gtao.slang")
        other_code = CHECKED.replace("X4000", "X3550")
        error = "shaders/tev.slang(10): error 20001: unexpected\n"
        self.assertEqual(slangc_quiet.filtered(other_file + other_code + error), other_file + other_code + error)

    def test_exit_code_and_output_pass_through(self):
        self.assertEqual(slangc_quiet.main([sys.executable, "-c", "import sys; sys.exit(3)"]), 3)


if __name__ == "__main__":
    unittest.main()
