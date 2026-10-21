"""python -I tools/step_audit [--generation DIR] [--output DIR] [--dumps GLOB] [--ignore-hooks] [--prove] [--apply]

Run from the repository root. Reads the active generation and existing RAM dumps; with
--apply writes game/pacing_fixes.inc and the step_audit blocks of project/hle_map.txt and project/uc.cfg, only for
records whose proof passed. Never runs the game.
"""
import argparse
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from pacing_hooks import BODY_HOOKS, HOOKS, hle_map_hooks  # noqa: E402
from step_audit import classes, empirical, fixes, ir, patterns, probe, program, report  # noqa: E402
from timing_audit import resolve_generation  # noqa: E402

S_480 = 0.0624


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--generation", type=Path, default=ROOT / "generated")
    p.add_argument("--output", type=Path, default=ROOT / "build" / "step_audit")
    p.add_argument("--dumps", default="build/autotest/episode*/dump_*.bin")
    p.add_argument("--ignore-hooks", action="store_true", help="report findings of hooked code as uncovered (validation)")
    p.add_argument("--prove", action="store_true")
    p.add_argument("--apply", action="store_true")
    args = p.parse_args(argv)
    t0 = time.time()
    generation = resolve_generation(args.generation)
    functions = ir.load_generation(args.generation)
    hle_map = ROOT / "project" / "hle_map.txt"
    code = set(functions) | classes.hle_entries(hle_map)
    dumps = sorted(ROOT.glob(args.dumps))
    latest = {}
    for d in dumps:
        latest[d.parent] = d                      # one dump per run is enough for vtables and objects
    index = classes.merge([classes.scan_dump(d, code) for d in latest.values()])
    entry_hooks = hle_map_hooks(hle_map, set(HOOKS))
    classes_of = program._classes(functions, index)
    offsets = empirical.vptr_offsets(functions, classes_of)
    measured = {}
    for low, high in empirical.dump_pairs(ROOT / "build" / "autotest"):
        for key, k in empirical.type_fields([(low.read_bytes()[:24 << 20], high.read_bytes()[:24 << 20])], 0x80000000, index, offsets, S_480).items():
            measured.setdefault(key, set()).add(k)
    measured = {key: next(iter(ks)) for key, ks in measured.items() if len(ks) == 1}
    pointers = empirical.pointer_types([d.read_bytes()[:24 << 20] for d in latest.values()], 0x80000000, index, offsets)
    print(f"loaded {len(functions)} functions, {len(index.vtables)} vtables, {len(measured)} measured fields, "
          f"{len(pointers)} typed pointers in {time.time() - t0:.0f}s", flush=True)
    prog = program.run_program(functions, index, patterns.HLE_SUMMARIES, empirical=measured, body_hooks=BODY_HOOKS,
                               entry_hooks=entry_hooks, progress=True, pointer_types=pointers)
    findings = patterns.find_patterns(prog)
    if args.ignore_hooks:
        for f in findings:
            f.covered_by = None
    records, rejected = fixes.propose(findings, prog)
    proofs = {}
    if args.prove and records:
        deny = probe.deny_list(hle_map)
        pairs = [(d, 1.0) for d in latest.values() if "-30-" in d.parent.name] + [(d, S_480) for d in latest.values() if "-480-" in d.parent.name]
        for r in records:
            proofs[(r.entry, r.lr)] = probe.prove(r, prog, pairs, offsets, deny)
            print(f"proof {r.entry} lr {r.lr}: {proofs[(r.entry, r.lr)].status} {proofs[(r.entry, r.lr)].reason}", flush=True)
    passed = [r for r in records if proofs.get((r.entry, r.lr)) and proofs[(r.entry, r.lr)].status == "pass"]
    statuses = {}
    rejected_by_site = {(x.function, x.store): x.reason for x in rejected}
    for f in findings:
        if f.covered_by:
            statuses[id(f)] = f"covered:{f.covered_by}"
        elif any(r.entry == f.function for r in passed):
            statuses[id(f)] = "auto-fixed"
        else:
            statuses[id(f)] = "report: " + rejected_by_site.get((f.function, f.store), "proof did not pass" if args.prove else "not proved")
    if args.apply:
        fixes.write_inc(passed, ROOT / "game" / "pacing_fixes.inc")
        fixes.sync_bindings(passed, hle_map, ROOT / "project" / "uc.cfg")
    stats = {"functions": len(functions), "vtables": len(index.vtables), "measured_fields": len(measured),
             "typed_fields": len(prog.field_k), "seconds": round(time.time() - t0)}
    data = report.write(args.output, generation.name, findings, statuses, records, proofs, stats)
    print(f"findings {len(findings)} {data['counts']} records {len(records)} passed {len(passed)} -> {args.output}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
