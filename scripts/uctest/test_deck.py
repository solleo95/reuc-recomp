import unittest
from unittest.mock import patch

from . import deck


class GameModeProcessTests(unittest.TestCase):
    def launch(self, env=None):
        with patch.dict(deck.os.environ, {}, clear=True), \
                patch.object(deck, "_free_port", side_effect=[31001, 31002]), \
                patch.object(deck, "_ssh"), patch.object(deck.subprocess, "Popen") as popen:
            deck.DeckRun("process-test")._start_in_game_mode([], env)
            return popen.call_args_list[0].args[0][-1]

    def test_default_process(self):
        launch = self.launch()
        self.assertEqual(launch.count("pgrep -x uc >/dev/null"), 2)

    def test_variant_process(self):
        launch = self.launch({"UC_EXE": "./uc_n1scalar"})
        self.assertEqual(launch.count("pgrep -x uc_n1scalar >/dev/null"), 2)
        self.assertIn("pkill -x uc;", launch)
        self.assertNotIn("pkill -x uc_n1scalar", launch)

    def test_path_and_regex_characters(self):
        launch = self.launch({"UC_EXE": "/home/deck/uc-run/uc.a[1]"})
        self.assertEqual(launch.count(r"pgrep -x 'uc\.a\[1\]' >/dev/null"), 2)

    def test_linux_comm_length(self):
        launch = self.launch({"UC_EXE": "./uc_12345678901234567890"})
        self.assertEqual(launch.count("pgrep -x uc_123456789012 >/dev/null"), 2)


if __name__ == "__main__":
    unittest.main()
