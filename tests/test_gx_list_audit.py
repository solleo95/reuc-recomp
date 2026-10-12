import importlib.util
import sys
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("gx_list_audit",Path(sys.argv.pop(1))/"tools/spikes/gx_list_audit.py")
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class AuditTest(unittest.TestCase):
    def test_state_words_are_never_scanned_for_fake_draws(self):
        data = bytes.fromhex("61 41 90 00 03 08 50 90 00 00 03 10 00 00 10 00 90 00 00 03 98 00 00 00")
        report = audit.inspect(data)
        self.assertTrue(report["complete"])
        self.assertEqual([c["kind"] for c in report["commands"]],["BP","CP","XF","primitive"])
        self.assertEqual(report["commands"][-1]["vertices"],0)

    def test_stop_before_nonempty_vertex_payload(self):
        report = audit.inspect(bytes.fromhex("90 00 03 FF FF FF"))
        self.assertFalse(report["complete"])
        self.assertEqual(report["payload_offset"],3)
        self.assertEqual(report["commands"][0]["vertices"],3)

    def test_malformed_capture_and_commands(self):
        for data in (bytes.fromhex("61 41 00"),bytes.fromhex("10 FF FF 10 00"),bytes.fromhex("90 00"),bytes.fromhex("FF")):
            with self.assertRaises(ValueError): audit.inspect(data)
        with self.assertRaises(ValueError): audit.resources_from_log("gx resource: address 80002000 size 4 captured 2 hex 61 41\n")
        with self.assertRaises(ValueError): audit.resources_from_log("gx resource: address 80002000 size 4 captured 4 hex 61 41\n")
        self.assertEqual(audit.resources_from_log("gx resource: address 80002000 size 2 captured 2 hex 61 41\n"),[(0x80002000,b"\x61\x41")])


if __name__ == "__main__":
    unittest.main()
