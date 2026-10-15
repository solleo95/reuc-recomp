"""Reads a crash bundle written by uc.exe (<report stem>.dump next to the crash-*.txt report): guest memory, thread registers, GX arrays, function table.

python -m uctest inspect <crash-*.txt | *.dump> threads | stack [thread id] | mem ADDR [SIZE] | f32 ADDR [COUNT] | nan ADDR SIZE | posmtx INDEX | gx
Addresses are hex, sizes and counts decimal. Layout: docs/testing/test-server.md ("Crash bundle")."""
import bisect
import math
import re
import struct
from pathlib import Path

MEM1, MEM1_SIZE, MEM2 = 0x80000000, 24 << 20, 0x90000000


class Bundle:
    def __init__(self, path):
        path = Path(path)
        self.dir = path if path.is_dir() else path.with_suffix(".dump")
        self.mem = {MEM1: (self.dir / "mem1.bin").read_bytes(), MEM2: (self.dir / "mem2.bin").read_bytes()}
        self._threads = None
        entries = []
        for line in (self.dir / "functions.txt").read_text().splitlines():
            address, _, name = line.partition(" ")
            entries.append((int(address, 16), name.strip()))
        self.functions = sorted(entries)
        self._starts = [address for address, _ in self.functions]

    def read(self, address, size):
        for base, data in self.mem.items():
            offset = address - base
            if 0 <= offset and offset + size <= len(data):
                return data[offset:offset + size]
        raise ValueError(f"{address:08X}+{size} is not inside the dumped guest memory")

    def u32(self, address):
        return struct.unpack(">I", self.read(address, 4))[0]

    def f32(self, address):
        return struct.unpack(">f", self.read(address, 4))[0]

    def threads(self):
        if self._threads is None:
            self._threads = []
            for line in (self.dir / "threads.txt").read_text().splitlines():
                head = re.match(r"thread (\d+) osthread ([0-9A-F]+) state (\w+) priority (\d+) waitkey ([0-9A-F]+) current (\d)", line)
                if head:
                    self._threads.append({"id": int(head[1]), "osthread": int(head[2], 16), "state": head[3], "priority": int(head[4]), "waitkey": int(head[5], 16),
                                          "current": head[6] == "1", "r": [0] * 32, "f": [0] * 32, "lr": 0, "ctr": 0, "cr": 0, "msr": 0})
                    continue
                if not self._threads:
                    continue
                thread = self._threads[-1]
                for key, value in re.findall(r"\b(r\d+|f\d+|lr|ctr|cr|msr)=([0-9A-F]+)", line):
                    if key[0] in "rf" and key[1:].isdigit():
                        thread[key[0]][int(key[1:])] = int(value, 16)
                    else:
                        thread[key] = int(value, 16)
        return self._threads

    def crashed_thread(self):
        return next((t for t in self.threads() if t["current"]), None)

    def function_at(self, address):
        """(name, offset) of the translated function containing `address`; unnamed entries are fn_<address>. None before the first function."""
        index = bisect.bisect_right(self._starts, address) - 1
        if index < 0:
            return None
        start, name = self.functions[index]
        return (name or f"fn_{start:08X}", address - start)

    def stack(self, thread):
        """[(return address, name, offset)] from the lr register and the PowerPC back chain starting at r1 (the return address of a frame sits in its caller's frame at +4)."""
        addresses, sp = [thread["lr"]], thread["r"][1]
        for _ in range(64):
            try:
                back = self.u32(sp)
                if back <= sp:
                    break
                addresses.append(self.u32(back + 4))
            except ValueError:
                break
            sp = back
        return [(a, *(self.function_at(a) or ("?", 0))) for a in addresses]

    def non_finite(self, address, size):
        words = struct.unpack(f">{size // 4}f", self.read(address, size // 4 * 4))
        return [address + 4 * i for i, value in enumerate(words) if not math.isfinite(value)]

    def array(self, number):
        for line in (self.dir / "gx.txt").read_text().splitlines():
            found = re.match(rf"live array {number} address ([0-9A-F]+) stride (\d+)", line)
            if found:
                return int(found[1], 16), int(found[2])
        raise ValueError(f"GX array {number} was not bound")

    def posmtx(self, index):
        base, stride = self.array(12)
        address = base + index * stride
        return address, list(struct.unpack(">12f", self.read(address, 48)))


def label(name, offset):
    return f"{name}+0x{offset:X}" if offset else name


def command(bundle, name, arguments):
    if name == "threads":
        for t in bundle.threads():
            print(f"thread {t['id']} osthread {t['osthread']:08X} {t['state']} priority {t['priority']} waitkey {t['waitkey']:08X}{' current (crashed)' if t['current'] else ''}  r1={t['r'][1]:08X} lr={t['lr']:08X}")
    elif name == "stack":
        thread = bundle.crashed_thread() if not arguments else next(t for t in bundle.threads() if t["id"] == int(arguments[0]))
        for number, (address, function, offset) in enumerate(bundle.stack(thread)):
            print(f"#{number} {address:08X} {label(function, offset)}")
    elif name == "mem":
        address, size = int(arguments[0], 16), int(arguments[1]) if len(arguments) > 1 else 64
        data = bundle.read(address, size)
        for row in range(0, size, 16):
            chunk = data[row:row + 16]
            print(f"{address + row:08X}  {chunk.hex(' ').upper():<47}  {''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)}")
    elif name == "f32":
        address, count = int(arguments[0], 16), int(arguments[1]) if len(arguments) > 1 else 12
        for i in range(count):
            value = bundle.f32(address + 4 * i)
            print(f"{address + 4 * i:08X}  {value!r}{'' if math.isfinite(value) else '  NON-FINITE'}")
    elif name == "nan":
        found = bundle.non_finite(int(arguments[0], 16), int(arguments[1]))
        print("\n".join(f"{a:08X}  {bundle.f32(a)!r}" for a in found) or "all finite")
    elif name == "posmtx":
        address, values = bundle.posmtx(int(arguments[0]))
        print(f"position matrix {arguments[0]} at {address:08X}{'  NON-FINITE' if not all(math.isfinite(v) for v in values) else ''}")
        for row in range(3):
            print("  " + " ".join(f"{v:12.5g}" for v in values[row * 4:row * 4 + 4]))
    elif name == "gx":
        print((bundle.dir / "gx.txt").read_text(), end="")
    else:
        raise SystemExit(f"unknown inspect command {name}")
    return 0
