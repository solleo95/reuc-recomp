"""Drives build/game/step_probe.exe: proves a FixRecord exact on dumped state, or says why it cannot."""
from dataclasses import dataclass
import json
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
PROBE = ROOT / "build" / "game" / "step_probe.exe"
CASES = "1/2,1/4,1/16,5/24,2/11,1/8"            # 60, 120, 480, 144, 165, 240 Hz against the 30 Hz nominal frame
ALLOWED_HLE = re.compile(r"^(Leaf_|PS|C_|Nop$|OSDisableInterrupts$|OSRestoreInterrupts$|OSEnableInterrupts$)")
ARG_REGS = [f"r{n}" for n in range(4, 11)] + [f"f{n}" for n in range(1, 9)]
NOMINAL_SECONDS = 1.0 / 30                      # probe clock: rate 60, target 2


@dataclass
class ProofResult:
    status: str          # pass | fail | unproven
    reason: str
    max_rel: float | None = None
    dump: str | None = None


def extra_inputs(function, operand):
    """Argument registers other than r3 and `operand` that the function reads before writing them."""
    written, needed = set(), []
    for insn in function.insns:
        if insn.op in ("call",):
            break
        for role, arg in zip(insn.roles, insn.args):
            name = f"{'f' if role[0] == 'F' else 'r'}{arg}" if role[0] in "FI" else None
            if not name:
                continue
            if role in ("Is", "Fs", "Ib", "Iw") and name in ARG_REGS and name != operand and name not in written and name not in needed:
                needed.append(name)
        for role, arg in zip(insn.roles, insn.args):
            if role in ("Id", "Fd", "Iw"):
                written.add(f"{'f' if role[0] == 'F' else 'r'}{arg}")
    return needed


def run_probe(command, timeout=600) -> ProofResult:
    try:
        done = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return ProofResult("unproven", "probe timed out")
    if done.returncode != 0:
        tail = (done.stderr or done.stdout).strip().splitlines()
        return ProofResult("unproven", f"probe exit {done.returncode}: {tail[0] if tail else ''}"[:300])
    line = next((l for l in reversed(done.stdout.splitlines()) if l.startswith("{")), None)
    if not line:
        return ProofResult("unproven", "probe printed no result")
    data = json.loads(line)
    return ProofResult(data["status"], data.get("reason", ""), data.get("max_rel"))


def deny_list(hle_map: Path):
    import sys
    sys.path.insert(0, str(ROOT / "tools"))
    from pacing_hooks import HOOKS
    deny = []
    for line in Path(hle_map).read_text(encoding="utf-8").splitlines():
        m = re.match(r"@([0-9A-Fa-f]{8})\s+(\w+)", line)
        if m and not ALLOWED_HLE.match(m[2]) and not m[2].startswith("PacingFix_") and m[2] not in HOOKS:
            deny.append(m[1].upper())
    return deny


def _word(raw, address):
    o = address - 0x80000000 if address < 0x90000000 else (24 << 20) + address - 0x90000000
    return struct.unpack_from(">I", raw, o)[0], struct.unpack_from(">f", raw, o)[0]


def prove(record, prog, dumps, vptr_offsets, deny, body=None, frame=0, probe=PROBE) -> ProofResult:
    """dumps: [(path, s_of_dump)], 30 Hz dumps first. body/frame: the entry is already hooked (validation of known fixes)."""
    fn = prog.functions.get(body or record.entry)
    if fn is None:
        return ProofResult("unproven", "function body not in the generation")
    needed = extra_inputs(fn, record.operand)
    if needed:
        return ProofResult("unproven", f"needs context: reads {', '.join(needed)} at entry")
    vt = prog.classes.get(record.entry) or prog.classes.get(body or "")
    if vt is None:
        return ProofResult("unproven", "no state: the function has no known class to take an object from")
    holders = prog.dump_index.objects.get(vt, [])
    k1 = [off for (base, off), k in prog.field_k.items() if base == ("class", vt) and k == 1]
    for path, s in dumps:
        raw = Path(path).read_bytes()
        for loc in holders:
            if _word(raw, loc)[0] != vt:
                continue
            this = loc - vptr_offsets.get(vt, 0)
            fields = ";".join(f"{this + off:08X}={_word(raw, this + off)[1] / s:.9g}" for off in k1)
            command = [str(probe), str(path), record.entry, record.lr, record.operand[1:], str(record.power), f"{this:08X}",
                       repr(NOMINAL_SECONDS), CASES, fields, ",".join(deny), body or "0", str(frame)]
            result = run_probe(command)
            result.dump = str(path)
            if result.status != "unproven" or "changed no state" not in result.reason:
                return result
    return ProofResult("unproven", "no state: no dumped object of the class exercises the site")
