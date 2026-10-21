"""Offline triage of persistent per-call updates in translated PPC; never patches code.

python -I tools/timing_audit.py
See docs/testing/timing-audit.md for coverage and interpretation.
"""
import argparse
from collections import Counter
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from leaf_hybrid import OP, ROLES
from pacing_hooks import BODY_HOOKS, HOOKS

ROOT = Path(__file__).resolve().parent.parent
FUNCTION = re.compile(r"^(?:static )?void (?:fn|body)_([0-9A-F]{8})\(ppc::Ctx& c(?:, uint32_t at)?\) \{")
INSTRUCTION = re.compile(r"/\*([0-9A-F]{8})\*/")
LOADS = {"lwz", "lbz", "lhz", "lha", "lfs", "lfd", "psq_l"}
STORES = {"stw", "stb", "sth", "stfs", "stfd", "psq_st"}
FLOAT_ADD = {"fadd", "fadds", "fsub", "fsubs", "ps_add", "ps_sub"}
FLOAT_MUL = {"fmul", "fmuls", "fdivs", "ps_mul", "ps_muls0", "ps_muls1"}
FLOAT_MADD = {"fmadd", "fmadds", "fmsub", "fmsubs", "fnmadds", "fnmsubs",
              "ps_madd", "ps_msub", "ps_madds0", "ps_madds1", "ps_nmadd"}
LIMITATIONS = (
    "Candidates are not proof of FPS bugs. Time dependency is not proof of correct scaling. "
    "Analysis is local to straight-line blocks: branches, calls, unknown instructions and pointer aliases can hide updates. "
    "No interprocedural time propagation, virtual-call resolution, indexed-memory analysis or host-C++ analysis. "
    "Event/refcount updates, matrix math and loop work can look like frame updates. "
    "HLE-replaced bodies absent from this generation are not scanned; hooks are annotations, never suppressions. "
    "Missing findings do not certify a function. Fixed-step solver arguments, constant assignments, collision thresholds "
    "and wrong dt powers can require manual analysis even without a recurrence."
)


@dataclass(frozen=True)
class Value:
    refs: frozenset = frozenset()
    ops: tuple = ()
    evidence: tuple = ()
    time: bool = False
    pointer: tuple | None = None


def combined(values, op, address):
    return Value(frozenset().union(*(v.refs for v in values)),
                 tuple(dict.fromkeys(o for v in values for o in v.ops)) + (op,),
                 tuple(sorted({a for v in values for a in v.evidence} | {address}))[-12:],
                 any(v.time for v in values))


def scan_function(function):
    registers = {}
    block = 0
    findings = []
    unsupported = Counter()
    instructions = function.pop("instructions")
    targets = {int(t, 16) for _, _, s in instructions for t in re.findall(r"goto L_([0-9A-F]{8})", s)}
    loops = [(int(t, 16), a) for a, _, s in instructions
             for t in re.findall(r"goto L_([0-9A-F]{8})", s) if int(t, 16) <= a]
    time_reads = []

    def reset():
        nonlocal block
        block += 1
        registers.clear()

    def reg(kind, n):
        if (kind, n) not in registers:
            pointer = None
            if kind == "I":
                pointer = ("stack", 0) if n == 1 else ("absolute", {2: 0x806CC700, 13: 0x806CA620}[n]) if n in (2, 13) else (f"r{n}@block{block}", 0)
            registers[kind, n] = Value(pointer=pointer)
        return registers[kind, n]

    def memory(base, offset):
        pointer = ("absolute", 0) if base == 0 else reg("I", base).pointer
        return (pointer[0], (pointer[1] + offset) & 0xFFFFFFFF) if pointer else None

    for address, line_no, text in instructions:
        if address in targets:
            reset()
        if re.search(r"(?:fn_[0-9A-F]+|hle_\w+)\(c\)", text) or "call_indirect" in text:
            reset()
            continue
        match = OP.search(text)
        if not match:
            if "goto " in text or "return;" in text:
                reset()
            elif "/* cr" not in text:
                unsupported["unparsed instruction"] += 1
                reset()
            continue
        op = match[1]
        roles = ("Id Is imm" if op in ("addic", "addic_") else ROLES.get(op, "")).split()
        args = match[2].split(",")
        if not roles or len(roles) != len(args):
            unsupported[op] += 1
            reset()
            continue
        try:
            args = [int(a.strip().rstrip("u"), 0) if a.strip() not in ("true", "false") else a.strip() == "true" for a in args]
        except ValueError:
            unsupported[op] += 1
            reset()
            continue
        values = [reg(role[0], n) for role, n in zip(roles, args) if role in ("Is", "Fs", "Iw")]
        dests = [(role[0], n) for role, n in zip(roles, args) if role in ("Id", "Fd", "Iw")]
        result = Value()
        if op in LOADS:
            key = memory(args[1], args[2])
            timed = key in (("absolute", 0x804D331C), ("absolute", 0x804D3320))
            if timed:
                time_reads.append(f"{address:08X}")
            result = Value(refs=frozenset([key]) if key else frozenset(), evidence=(address,), time=timed,
                           pointer=(f"load@{address:08X}", 0) if roles[0] == "Id" else None)
        elif op in STORES:
            key = memory(args[1], args[2])
            value = reg(roles[0][0], args[0])
            if key and key[0] != "stack" and key in value.refs and value.ops:
                last_op = value.ops[-1]
                kind = "integer-step" if roles[0] == "Is" else "float-multiply" if last_op in FLOAT_MUL else "float-add"
                in_loop = any(lo <= address <= hi for lo, hi in loops)
                priority = "review-time-use" if value.time else "inspect-loop" if in_loop else "high" if kind != "integer-step" else "medium"
                findings.append(dict(address=f"{address:08X}", line=line_no, field=f"{key[0]}+0x{key[1]:X}",
                                     kind=kind, operations=list(value.ops), time_dependency=value.time,
                                     priority=priority, in_loop=in_loop,
                                     evidence=[f"{a:08X}" for a in value.evidence] + [f"{address:08X}"]))
            continue
        elif op in ("addi", "addis", "addic", "addic_", "stwu"):
            src = reg("I", args[1]) if args[1] or op in ("addic", "addic_") else Value(pointer=("absolute", 0))
            amount = args[2] * (65536 if op == "addis" else 1)
            result = combined([src], op, address) if amount else src
            pointer = (src.pointer[0], (src.pointer[1] + amount) & 0xFFFFFFFF) if src.pointer else None
            result = Value(result.refs, result.ops, result.evidence, result.time, pointer)
        elif op == "or" and args[0] == args[2]:
            result = reg("I", args[0])
        elif op in ("fmr", "ps_mr"):
            result = reg("F", args[1])
        elif op in FLOAT_ADD | FLOAT_MUL | FLOAT_MADD | {"add", "subf", "mulli", "mullw"}:
            result = combined(values, op, address)
        elif op in ("fneg", "fabs", "frsp", "ps_neg", "ps_abs"):
            # Preserve the recurrence, but these unary operations alone are not a timestep update.
            result = values[0]
        elif dests or op.startswith("st"):
            unsupported[op] += 1
        for dest in dests:
            registers[dest] = result
    function.update(findings=findings, time_reads=time_reads, unsupported=dict(sorted(unsupported.items())),
                    instruction_count=len(instructions), block_count=block + 1)
    return function


def scan_source(text, path):
    functions = []
    current = None
    for line_no, line in enumerate(text.splitlines(), 1):
        match = FUNCTION.match(line)
        if match:
            if current and current["instructions"]:
                functions.append(scan_function(current))
            current = dict(address=match[1], path=str(path), line=line_no, instructions=[],
                           callees=set(), hle_calls=set(), indirect_calls=0)
        instruction = INSTRUCTION.search(line)
        if current and instruction:
            current["instructions"].append((int(instruction[1], 16), line_no, line))
            current["callees"].update(re.findall(r"fn_([0-9A-F]{8})\(c\)", line))
            current["hle_calls"].update(re.findall(r"hle_(\w+)\(c\)", line))
            current["indirect_calls"] += "call_indirect" in line
    if current and current["instructions"]:
        functions.append(scan_function(current))
    if not functions or not any(f["instruction_count"] for f in functions):
        raise ValueError(f"No translated instructions in {path}; unsupported format or empty input")
    for function in functions:
        for name in ("callees", "hle_calls"):
            function[name] = sorted(function[name])
    return functions


def resolve_generation(directory):
    manifest = directory / "game_sources.cmake"
    if manifest.exists():
        match = re.search(r'UC_GENERATED_INCLUDE_DIR\s+"\$\{CMAKE_CURRENT_LIST_DIR\}/(generation_[\w]+)"', manifest.read_text(encoding="utf-8"))
        if not match:
            raise ValueError(f"Cannot resolve active generation from {manifest}")
        directory = directory / match[1]
    if not directory.is_dir() or not list(directory.glob("game_[0-9]*.cpp")):
        raise ValueError(f"No generated shards in {directory}; run build.bat first")
    return directory


def make_report(functions, hle):
    callers = {}
    for function in functions:
        for callee in function["callees"]:
            callers.setdefault(callee, []).append(function["address"])
    active_names = set(hle.values())
    for function in functions:
        function["callers"] = sorted(set(callers.get(function["address"], [])))
        hook = BODY_HOOKS.get(function["address"])
        function["body_hook"] = hook if hook in active_names else None
    hooks = [dict(address=a, name=n, coverage=HOOKS.get(n, "native replacement; not analyzed"),
                  callers=[f["address"] for f in functions if n in f["hle_calls"]]) for a, n in sorted(hle.items())]
    counts = Counter(r["priority"] for f in functions for r in f["findings"])
    unsupported = Counter()
    for function in functions:
        unsupported.update(function["unsupported"])
    return dict(schema=1, limitations=LIMITATIONS, summary=dict(functions=len(functions),
                instructions=sum(f["instruction_count"] for f in functions),
                findings=sum(counts.values()), priorities=dict(sorted(counts.items())),
                indirect_calls=sum(f["indirect_calls"] for f in functions), unsupported=dict(sorted(unsupported.items()))),
                hooks=hooks, functions=functions)


def markdown(report, focus=None):
    summary = {k: v for k, v in report["summary"].items() if k != "unsupported"}
    summary["unmodeled_operations"] = sum(report["summary"]["unsupported"].values())
    lines = ["# Timing audit", "", report["limitations"], "", "```json", json.dumps(summary, indent=2), "```", "",
             "Priorities: high = float recurrence without recognized dt; medium = integer recurrence; "
             "inspect-loop = backward branch covers the store; review-time-use = dt reaches expression, correctness still unproven.", "",
             "## Existing timing hooks", ""]
    if focus:
        lines[2:2] = ["Focused function ranges: " + ", ".join(f"{lo:08X}:{hi:08X}" for lo, hi in focus) + ". Summary counts cover the entire generation.", ""]
    for hook in report["hooks"]:
        if hook["name"] in HOOKS:
            lines.append(f'- `{hook["address"]}` {hook["name"]}: {hook["coverage"]}.')
    lines += ["", "## Candidates", ""]
    order = {"high": 0, "medium": 1, "inspect-loop": 2, "review-time-use": 3}
    functions = sorted(report["functions"], key=lambda f: (min((order[r["priority"]] for r in f["findings"]), default=4), f["address"]))
    for function in functions:
        if not function["findings"] or focus and not any(lo <= int(function["address"], 16) <= hi for lo, hi in focus):
            continue
        lines += [f'### {function["address"]}', "", f'Source: [{Path(function["path"]).name}:{function["line"]}](<{function["path"]}:{function["line"]}>).',
                  f'Direct callers: {", ".join(function["callers"]) or "none resolved"}. Indirect calls: {function["indirect_calls"]}.',
                  f'HLE calls: {", ".join(function["hle_calls"]) or "none"}. Body hook: {function["body_hook"] or "none"}.', ""]
        for row in function["findings"]:
            lines.append(f'- **{row["priority"]}** `{row["address"]}` {row["kind"]}, `{row["field"]}`; '
                         f'ops={",".join(row["operations"])}; dt={row["time_dependency"]}; loop={row["in_loop"]}. '
                         f'Evidence: {" → ".join(row["evidence"])}.')
        lines.append("")
    return "\n".join(lines)


def address_range(value):
    parts = value.split(":")
    try:
        if len(parts) != 2:
            raise ValueError()
        lo, hi = (int(p, 16) for p in parts)
        if not 0 <= lo <= hi <= 0xFFFFFFFF:
            raise ValueError()
        return lo, hi
    except ValueError:
        raise argparse.ArgumentTypeError("expected inclusive hexadecimal START:END") from None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generation", type=Path, default=ROOT / "generated")
    parser.add_argument("--output", type=Path, default=ROOT / "build/timing_audit")
    parser.add_argument("--focus", type=address_range, action="append", help="also write focus.md for function-address range START:END")
    args = parser.parse_args()
    try:
        generation = resolve_generation(args.generation.resolve())
        functions = []
        digest = hashlib.sha256()
        for path in sorted(generation.glob("game_[0-9]*.cpp")):
            raw = path.read_bytes()
            digest.update(path.name.encode() + b"\0" + raw)
            functions.extend(scan_source(raw.decode("utf-8"), path.as_posix()))
        hle_raw = (generation / "game_table.cpp").read_bytes()
        hle = dict(re.findall(r"\{0x([0-9A-F]{8})u, hle_(\w+)\}", hle_raw.decode("utf-8")))
        report = make_report(functions, hle)
        report["provenance"] = dict(generation=generation.as_posix(), source_sha256=digest.hexdigest(),
                                    hle_sha256=hashlib.sha256(hle_raw).hexdigest(),
                                    analyzer_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                                    opcode_roles_sha256=hashlib.sha256(Path(__file__).with_name("leaf_hybrid.py").read_bytes()).hexdigest(),
                                    focus=args.focus)
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        (args.output / "report.md").write_text(markdown(report), encoding="utf-8")
        if args.focus:
            (args.output / "focus.md").write_text(markdown(report, args.focus), encoding="utf-8")
        print(json.dumps(report["summary"], indent=2))
        print(f"Reports: {args.output.resolve()}")
    except (OSError, ValueError) as error:
        parser.exit(1, f"timing_audit: {error}\n")


if __name__ == "__main__":
    main()
