import contextlib, io, struct, sys, tempfile, unittest
from pathlib import Path
sys.path.insert(0, str(Path(sys.argv[1]) / "scripts"))
from uctest import dumpview
from uctest.__main__ import main

NAN = float("nan")


def make_bundle(root):
    """A crash bundle in miniature: two stack frames, a posmtx array whose entry 2 holds a NaN, three functions."""
    dump = root / "crash-1.dump"
    dump.mkdir()
    mem1 = bytearray(0x1000)
    def put32(address, value): struct.pack_into(">I", mem1, address - 0x80000000, value)
    put32(0x80000100, 0x80000200); put32(0x80000204, 0x80003000)   # frame of the crashed function: back chain, saved lr
    put32(0x80000200, 0x80000300); put32(0x80000304, 0x80004010)   # its caller's frame
    put32(0x80000300, 0)                                           # end of the chain
    for index in range(3):                                         # array 12 at 80000400, stride 48
        values = [1.0] * 12
        if index == 2: values[5] = NAN
        struct.pack_into(">12f", mem1, 0x400 + index * 48, *values)
    (dump / "mem1.bin").write_bytes(bytes(mem1))
    (dump / "mem2.bin").write_bytes(bytes(0x100))
    regs = "".join(f"r{i}={0x80000100 if i == 1 else i:08X}{chr(10) if i % 8 == 7 else ' '}" for i in range(32))
    fregs = "".join(f"f{i}={0:016X}{chr(10) if i % 4 == 3 else ' '}" for i in range(32))
    (dump / "threads.txt").write_text(
        "thread 1 osthread 80123400 state waiting priority 16 waitkey 00000000 current 0\n" + regs + "lr=80001000 ctr=00000000 cr=00000000 msr=00000000\n" + fregs +
        "thread 2 osthread 80567800 state running priority 10 waitkey 00000000 current 1\n" + regs + "lr=80002020 ctr=00000000 cr=00000000 msr=00000000\n" + fregs)
    (dump / "gx.txt").write_text("live array 12 address 80000400 stride 48\nlive array 13 address 80000800 stride 36\n")
    (dump / "functions.txt").write_text("80003000 foo\n80004000 bar\n80005000 \n")
    report = root / "crash-1.txt"
    report.write_text("reason: test\n")
    return report, dump


class DumpViewTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.report, self.dump = make_bundle(Path(self.tmp.name))
        self.bundle = dumpview.Bundle(self.report)   # the report path finds the folder next to it

    def test_reads_guest_memory_and_rejects_unmapped(self):
        self.assertEqual(self.bundle.u32(0x80000100), 0x80000200)
        self.assertEqual(self.bundle.f32(0x80000400), 1.0)
        with self.assertRaises(ValueError):
            self.bundle.read(0x80000FFE, 8)      # past the end of MEM1
        with self.assertRaises(ValueError):
            self.bundle.read(0x10000000, 4)      # not guest memory

    def test_threads_mark_the_crashed_one(self):
        threads = self.bundle.threads()
        self.assertEqual([t["id"] for t in threads], [1, 2])
        crashed = self.bundle.crashed_thread()
        self.assertEqual((crashed["id"], crashed["r"][1], crashed["lr"]), (2, 0x80000100, 0x80002020))

    def test_function_lookup_names_unknown_entries(self):
        self.assertEqual(self.bundle.function_at(0x80004010), ("bar", 0x10))
        self.assertEqual(self.bundle.function_at(0x80005008), ("fn_80005000", 8))
        self.assertIsNone(self.bundle.function_at(0x80000010))

    def test_stack_walks_the_back_chain(self):
        frames = self.bundle.stack(self.bundle.crashed_thread())
        self.assertEqual([f[0] for f in frames], [0x80002020, 0x80003000, 0x80004010])   # lr register, then the saved return addresses
        self.assertEqual(frames[1][1:], ("foo", 0))
        self.assertEqual(frames[2][1:], ("bar", 0x10))

    def test_nan_and_posmtx(self):
        self.assertEqual(self.bundle.non_finite(0x80000400, 144), [0x80000400 + 2 * 48 + 5 * 4])
        address, values = self.bundle.posmtx(2)
        self.assertEqual(address, 0x80000400 + 2 * 48)
        self.assertEqual(sum(v != v for v in values), 1)

    def test_cli(self):
        for arguments, expected in ((["stack"], "bar+0x10"), (["threads"], "current"), (["nan", "80000400", "144"], f"{0x80000400 + 2 * 48 + 5 * 4:08X}"), (["posmtx", "2"], "NON-FINITE"), (["mem", "80000100", "8"], "80 00 02 00")):
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(main(["inspect", str(self.report), *arguments]), 0)
            self.assertIn(expected, out.getvalue(), arguments)


if __name__ == "__main__":
    sys.argv = [sys.argv[0]] + sys.argv[2:]
    unittest.main()
