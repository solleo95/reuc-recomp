"""Exercise the actual Dolphin callback without starting a desktop process."""
import ast
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

root = Path(sys.argv.pop(1))
spec = importlib.util.spec_from_file_location("dolphin_reference", root / "scripts/dolphin_reference.py")
reference = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reference)


class CaptureTest(unittest.TestCase):
    def callback(self, output):
        callbacks, pauses = [], []
        dolphin = types.ModuleType("dolphin")
        dolphin.event = types.SimpleNamespace(on_framedrawn=callbacks.append)
        dolphin.emulation = types.SimpleNamespace(pause=lambda: pauses.append(True))
        tree = ast.parse(reference.capture_script(output))
        self.assertIsInstance(tree.body[-1], ast.While)
        tree.body.pop()  # Event loop belongs to Dolphin; exercise its registered callback here.
        with patch.dict(sys.modules, dolphin=dolphin):
            exec(compile(tree, "capture.py", "exec"), {})
        self.assertEqual(len(callbacks), 1)
        return callbacks[0], pauses

    def test_six_frames_then_pause_without_overwriting(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            callback, pauses = self.callback(output)
            for i in range(6):
                callback(2, 1, bytes([i]) * 6)
            callback(2, 1, b"ignored")
            self.assertEqual(pauses, [True])
            self.assertEqual(json.loads((output / "complete.json").read_text()),
                             {"frames": 6, "width": 2, "height": 1})
            self.assertEqual(len(list(output.glob("*.ppm"))), 6)
            for i in range(6):
                self.assertEqual((output / f"frame-{i}.ppm").read_bytes(), b"P6\n2 1\n255\n" + bytes([i]) * 6)

    def test_bad_frame_fails_before_writing(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            callback, pauses = self.callback(output)
            with self.assertRaisesRegex(RuntimeError, "byte count"):
                callback(2, 1, b"short")
            self.assertEqual(list(output.iterdir()), [])
            self.assertEqual(pauses, [])


if __name__ == "__main__":
    unittest.main()
