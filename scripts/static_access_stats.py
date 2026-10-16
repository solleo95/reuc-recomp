"""python scripts/static_access_stats.py [generated dir]

Static estimate for the translator: how many loads and stores of the generated code have a base register that is a known constant (lis/addi in the
same straight-line stretch), and so need no MMIO test, versus an unknown pointer. Knowledge is dropped at every label and (except r1, r2, r13 and the
callee-saved r14-r31) at every call, so the figures are conservative."""
import collections
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
gen_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else None
if gen_dir is None:
    cmake = (ROOT / "generated" / "game_sources.cmake").read_text()
    gen_dir = ROOT / "generated" / re.search(r"generation_[0-9a-f_]+", cmake)[0]

LOADS = {"lwz", "lbz", "lhz", "lha", "lfs", "lfd", "lwzu", "lbzu", "lhzu", "lfsu", "lfdu", "psq_l"}
STORES = {"stw", "stb", "sth", "stfs", "stfd", "stwu", "stbu", "sthu", "stfsu", "stfdu", "psq_st"}
UPDATE = {"lwzu", "lbzu", "lhzu", "lfsu", "lfdu", "stwu", "stbu", "sthu", "stfsu", "stfdu"}
FP_DEST = {"lfs", "lfd", "lfsu", "lfdu"}   # first operand is an FPR
OP = re.compile(r"ppc::op_(\w+)\(c,\s*([^)]*)\)")
CALL = re.compile(r"\bfn_[0-9A-F]+\(c\)|call_indirect")
LABEL = re.compile(r"^L_[0-9A-F]+:;")


def args_of(text):
    return [a.strip() for a in text.split(",")]


def to_int(text):
    try:
        return int(text.rstrip("uU"), 0)
    except ValueError:
        return None


def region(address):
    address &= 0xFFFFFFFF
    if 0x80000000 <= address < 0x81800000 or 0x90000000 <= address < 0x94000000 or address < 0x01800000:
        return "RAM"
    if 0xCC000000 <= address < 0xCE000000:
        return "MMIO"
    return "other"


counts = collections.Counter()
for path in sorted(gen_dir.glob("game_*.cpp")):
    known = {}
    for line in path.read_text(errors="replace").split("\n"):
        stripped = line.strip()
        if LABEL.match(stripped):
            known = {}
            continue
        if stripped.startswith("void fn_"):
            known = {}
            continue
        if CALL.search(stripped):
            known = {r: v for r, v in known.items() if r in (1, 2, 13) or r >= 14}
        m = OP.search(stripped)
        if not m:
            continue
        op, operands = m.group(1), args_of(m.group(2))
        if op in LOADS or op in STORES:
            kind = "load" if op in LOADS else "store"
            base = to_int(operands[1]) if len(operands) > 1 else None
            displacement = to_int(operands[2]) if len(operands) > 2 else None
            if op in ("psq_l", "psq_st"):   # psq_l(c, frD, d, rA, w, i): the base is the third operand
                base = to_int(operands[2]) if len(operands) > 2 else None
                displacement = to_int(operands[1]) if len(operands) > 1 else None
            if base in (1, 2, 13):
                counts[(kind, "already cheap (r1/r2/r13)")] += 1
            elif base == 0:
                counts[(kind, "absolute address")] += 1
            elif base in known and displacement is not None:
                counts[(kind, "constant " + region(known[base] + displacement))] += 1
            else:
                counts[(kind, "unknown pointer")] += 1
            if op in UPDATE and base is not None:
                known.pop(base, None)
            if kind == "load" and op not in FP_DEST and op != "psq_l":
                destination = to_int(operands[0])
                if destination is not None:
                    known.pop(destination, None)
            continue
        if op == "addis" and len(operands) == 3 and to_int(operands[1]) == 0:
            destination, immediate = to_int(operands[0]), to_int(operands[2])
            if destination is not None and immediate is not None:
                known[destination] = (immediate << 16) & 0xFFFFFFFF
            continue
        if op == "addi" and len(operands) == 3:
            destination, source, immediate = to_int(operands[0]), to_int(operands[1]), to_int(operands[2])
            if destination is not None and immediate is not None:
                if source == 0:
                    known[destination] = immediate & 0xFFFFFFFF
                elif source in known:
                    known[destination] = (known[source] + immediate) & 0xFFFFFFFF
                else:
                    known.pop(destination, None)
            continue
        if op == "ori" and len(operands) == 3:   # ori(c, rA, rS, imm): rA = rS | imm
            destination, source, immediate = to_int(operands[0]), to_int(operands[1]), to_int(operands[2])
            if destination is not None:
                if source in known and immediate is not None:
                    known[destination] = known[source] | immediate
                else:
                    known.pop(destination, None)
            continue
        # any other integer instruction that names a destination first: forget it (stores, compares and branches name sources)
        destination = to_int(operands[0]) if operands else None
        if destination is not None and not op.startswith(("st", "cmp", "mt", "b", "fc", "cr", "psq_st", "dcb", "icbi", "sync", "isync")):
            known.pop(destination, None)

for kind in ("load", "store"):
    total = sum(n for (k, _), n in counts.items() if k == kind)
    print(f"{kind}s: {total} sites")
    for (k, label), n in sorted(counts.items(), key=lambda item: -item[1]):
        if k == kind:
            print(f"  {100 * n / total:5.1f}%  {n:7d}  {label}")
