import json, queue, sys, tempfile, threading, time, unittest
from pathlib import Path
sys.path.insert(0, str(Path(sys.argv[1]) / "scripts"))
from uctest.client import Game, StepFailed

class FakeTransport:
    def __init__(self):
        self.out = queue.Queue(); self.sent = []; self.closed = False
    def send_line(self, line):
        self.sent.append(line)
        cmd = json.loads(line)
        if cmd["cmd"] == "input": self.out.put(json.dumps({"id": cmd["id"], "ok": True, "consumed_frame": 12}))
        elif cmd["cmd"] == "capture": self.out.put(json.dumps({"id": cmd["id"], "ok": True, "path": "x.png"}))
        elif cmd["cmd"] == "speed": self.out.put(json.dumps({"id": cmd["id"], "ok": True, "factor": cmd["factor"]}))
    def read_line(self):
        return self.out.get()
    def close(self): self.closed = True; self.out.put(None)
    def frame(self, n, context="menu", **extra):
        self.out.put(json.dumps({"type": "frame", "n": n, "context": context, "speed": 1, "ui": [-1]*12, **extra}))

class ClientTest(unittest.TestCase):
    def test_launch_keeps_saved_borderless_config_windowed(self):
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); (root / "source-saves").mkdir(); (root / "run").mkdir()
            exe=root / "uc.exe"
            (root / "config.json").write_text(json.dumps({"graphics":{"window_mode":"borderless"}}))
            g=Game(exe=str(exe),run_dir=root / "run",saves=root / "source-saves")
            with patch("uctest.client.subprocess.Popen") as launch:
                g._launch()
            command=launch.call_args.args[0]
            self.assertEqual(command[command.index("--window-mode")+1],"windowed")
            self.assertEqual(command[command.index("--width")+1],"1280")
            self.assertEqual(command[command.index("--height")+1],"720")
    def game(self, t):
        return Game(transport_factory=lambda: t, launch=False, run_dir=Path(self.tmp.name))
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
    def tearDown(self): self.tmp.cleanup()
    def test_press_waits_for_consumption(self):
        t = FakeTransport()
        with self.game(t) as g:
            self.assertEqual(g.press("A"), 12)
            self.assertIn('"buttons": 2048', t.sent[-1])
    def test_wait_on_state(self):
        t = FakeTransport()
        with self.game(t) as g:
            threading.Timer(0.05, lambda: [t.frame(1), t.frame(2, "gameplay")]).start()
            self.assertEqual(g.wait(lambda f: f["context"] == "gameplay", seconds=2)["n"], 2)
    def test_wait_frame_budget(self):
        t = FakeTransport()
        with self.game(t) as g:
            for n in range(1, 40): t.frame(n)
            with self.assertRaises(StepFailed) as e: g.wait(lambda f: f["context"] == "gameplay", frames=10, seconds=2, awaited="gameplay")
            self.assertEqual(e.exception.artifacts["awaited"], "gameplay")
    def test_wait_aborts_on_exit(self):
        t = FakeTransport()
        with self.game(t) as g:
            start = time.monotonic(); threading.Timer(0.05, t.close).start()
            with self.assertRaises(StepFailed): g.wait(lambda f: False, seconds=30)
            self.assertLess(time.monotonic() - start, 5)
    def test_error_reply_raises(self):
        t = FakeTransport(); t.send_line = lambda line: t.out.put('{"id":1,"error":"bad"}')
        with self.game(t) as g:
            with self.assertRaises(StepFailed): g.request({"cmd": "state"})

if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
