"""Instruction-level view of translated game shards (generated/generation_*/game_*.cpp)."""
from dataclasses import dataclass, field
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from leaf_hybrid import OP, ROLES  # noqa: E402
from timing_audit import FUNCTION, INSTRUCTION, resolve_generation  # noqa: E402

CALL = re.compile(r"\bfn_([0-9A-F]{8})\(c\)")
HLE = re.compile(r"\bhle_(\w+)\(c\)")
GOTO = re.compile(r"goto L_([0-9A-F]{8})")
LABEL = re.compile(r"^L_([0-9A-F]{8}):;")
ROLE_OVERRIDES = {"addic": "Id Is N", "addic_": "Id Is N"}


@dataclass(frozen=True)
class Insn:
    address: int
    op: str
    args: tuple
    roles: tuple
    line: int
    call: str | None = None
    indirect_slot: int | None = None
    branch: tuple = ()
    returns: bool = False
    conditional: bool = False


@dataclass
class Function:
    address: str
    path: str
    insns: list = field(default_factory=list)
    labels: set = field(default_factory=set)
    prologue_frame: int | None = None


def _int(text):
    text = text.strip()
    if text in ("true", "false"):
        return text == "true"
    return int(text.rstrip("u"), 0)


def _insn(address, line_no, text, previous):
    branch = tuple(int(t, 16) for t in GOTO.findall(text))
    returns = "return;" in text
    conditional = "if (" in text or "--c.ctr" in text
    call = CALL.search(text)
    hle = HLE.search(text)
    callee = call[1] if call else f"hle:{hle[1]}" if hle else None
    slot = None
    if "call_indirect" in text:
        # lwz r12,N(r12) loads the slot; mtspr ctr follows it.
        for p in reversed(previous[-3:]):
            if p.op == "lwz" and p.args[:2] == (12, 12):
                slot = p.args[2]
                break
    match = OP.search(text)
    if not match:
        op = "call" if callee or "call_indirect" in text else "branch" if branch or returns or "/* cr" in text else "?"
        return Insn(address, op, (), (), line_no, callee, slot, branch, returns, conditional)
    op = match[1]
    roles = tuple(ROLE_OVERRIDES.get(op, ROLES.get(op, "")).split())
    try:
        args = tuple(_int(a) for a in match[2].split(",")) if match[2].strip() else ()
    except ValueError:
        return Insn(address, "?", (), (), line_no, callee, slot, branch, returns, conditional)
    if roles and len(roles) != len(args):
        roles = ()
    return Insn(address, op, args, roles, line_no, callee, slot, branch, returns, conditional)


def parse_shard(text: str, path: str) -> dict:
    functions = {}
    current = None
    for line_no, line in enumerate(text.splitlines(), 1):
        match = FUNCTION.match(line)
        if match:
            current = Function(match[1], str(path))
            functions[match[1]] = current
            continue
        if current is None:
            continue
        label = LABEL.match(line.strip())
        if label:
            current.labels.add(int(label[1], 16))
            continue
        instruction = INSTRUCTION.search(line)
        if instruction:
            insn = _insn(int(instruction[1], 16), line_no, line, current.insns)
            if not current.insns and insn.op == "stwu" and insn.args[:2] == (1, 1) and insn.args[2] < 0:
                current.prologue_frame = -insn.args[2]
            current.insns.append(insn)
    return {a: f for a, f in functions.items() if f.insns}


def load_generation(directory: Path) -> dict:
    functions = {}
    for shard in sorted(resolve_generation(Path(directory)).glob("game_[0-9]*.cpp")):
        functions.update(parse_shard(shard.read_text(encoding="utf-8"), shard))
    return functions
