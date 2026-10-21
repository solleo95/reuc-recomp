"""Vtables and object locations found in guest RAM dumps (MEM1 24 MiB at 0x80000000, then MEM2 64 MiB at 0x90000000)."""
from dataclasses import dataclass, field
import hashlib
import json
from pathlib import Path
import re
import struct

MEM1, MEM2 = 24 << 20, 64 << 20
CACHE = Path(__file__).resolve().parents[2] / "build" / "step_audit" / "cache"
HEAD = 6          # a vtable base: its first HEAD words are 0 or code pointers, at least two of them code
MAX_SLOTS = 256


@dataclass
class DumpIndex:
    vtables: dict = field(default_factory=dict)   # base -> [entry hex or None per 4-byte slot]
    objects: dict = field(default_factory=dict)   # base -> [addresses of words holding the base (vptr locations)]

    def slot_targets(self, slot: int) -> set:
        i = slot // 4
        return {t[i] for t in self.vtables.values() if i < len(t) and t[i]}


def hle_entries(hle_map: Path) -> set:
    return {m[1].upper() for m in re.finditer(r"^@([0-9A-Fa-f]{8})\s", Path(hle_map).read_text(encoding="utf-8"), re.M)}


def scan_buffer(buf: bytes, base: int, functions: set) -> DumpIndex:
    code = {int(f, 16) for f in functions}
    values = struct.unpack(">%dI" % (len(buf) // 4), buf[: len(buf) // 4 * 4])
    end = base + 4 * len(values)

    def word(address):
        return values[(address - base) // 4] if base <= address < end else None

    index = DumpIndex()
    candidates = {}
    for i, v in enumerate(values):
        if v and base <= v < end and v % 4 == 0:
            candidates.setdefault(v, []).append(base + 4 * i)
    for v, holders in candidates.items():
        head = [word(v + 4 * k) for k in range(HEAD)]
        if None in head or sum(h in code for h in head) < 2 or any(h and h not in code for h in head):
            continue
        slots = []
        for k in range(MAX_SLOTS):
            w = word(v + 4 * k)
            if w is None or (w and w not in code):
                break
            slots.append(f"{w:08X}" if w else None)
        index.vtables[v] = slots
        index.objects[v] = sorted(holders)
    return index


def merge(indexes: list) -> DumpIndex:
    out = DumpIndex()
    for ix in indexes:
        for v, slots in ix.vtables.items():
            mine = out.vtables.setdefault(v, [])
            for i, s in enumerate(slots):
                if i >= len(mine):
                    mine.append(s)
                elif mine[i] is None:
                    mine[i] = s
        for v, holders in ix.objects.items():
            out.objects[v] = sorted(set(out.objects.get(v, [])) | set(holders))
    return out


def scan_dump(path: Path, functions: set) -> DumpIndex:
    path = Path(path)
    key = hashlib.sha1(f"{path.resolve()}|{path.stat().st_mtime_ns}|{len(functions)}".encode()).hexdigest()
    cached = CACHE / f"{key}.json"
    if cached.exists():
        data = json.loads(cached.read_text(encoding="utf-8"))
        return DumpIndex({int(k): v for k, v in data["vtables"].items()}, {int(k): v for k, v in data["objects"].items()})
    raw = path.read_bytes()
    index = merge([scan_buffer(raw[:MEM1], 0x80000000, functions), scan_buffer(raw[MEM1:MEM1 + MEM2], 0x90000000, functions)])
    CACHE.mkdir(parents=True, exist_ok=True)
    cached.write_text(json.dumps({"vtables": index.vtables, "objects": index.objects}), encoding="utf-8")
    return index
