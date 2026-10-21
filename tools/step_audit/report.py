"""report.json / report.md for a step_audit run."""
from collections import Counter
from dataclasses import asdict
import json
from pathlib import Path

LIMITS = ("Static step-exponent analysis of the translated game. Findings are candidates with evidence, not proven bugs, "
          "except entries marked auto-fixed (proven exact by step_probe). Unresolved virtual calls, aliasing, indexed memory and "
          "per-frame context outside one function make values unknown, never safe.")


def _f(finding, status):
    d = asdict(finding)
    d["store"] = f"{finding.store:08X}" if finding.store is not None else None
    d["chain"] = [f"{a:08X}" for a in finding.chain]
    d["field"] = str(finding.field)
    d["sources"] = sorted(finding.sources)
    d["ctx"] = list(finding.ctx)
    d["status"] = status
    return d


def write(output: Path, generation: str, findings, statuses, records, proofs, stats):
    output.mkdir(parents=True, exist_ok=True)
    rows = [_f(f, statuses.get(id(f), "report")) for f in findings]
    data = {"schema": 1, "generation": generation, "limitations": LIMITS, "stats": stats,
            "counts": dict(Counter(r["status"].split(":")[0] for r in rows)),
            "by_pattern": dict(Counter(f"{r['pattern']}/{r['confidence']}" for r in rows)),
            "findings": rows,
            "records": [dict(asdict(r), proof=asdict(proofs[(r.entry, r.lr)]) if (r.entry, r.lr) in proofs else None) for r in records]}
    (output / "report.json").write_text(json.dumps(data, indent=1), encoding="utf-8")
    md = [f"# Step audit — {generation}", "", LIMITS, "", "| Status | Count |", "| --- | ---: |"]
    md += [f"| {k} | {v} |" for k, v in sorted(data["counts"].items())]
    md += ["", "| Pattern/confidence | Count |", "| --- | ---: |"] + [f"| {k} | {v} |" for k, v in sorted(data["by_pattern"].items())]
    header = ["| Status | Pattern | Conf. | Function | Store | Field | Detail |", "| --- | --- | --- | --- | --- | --- | --- |"]
    line = lambda r: f"| {r['status']} | {r['pattern']} | {r['confidence']} | {r['function']} | {r['store'] or ''} | `{r['field']}` | {r['detail']} |"
    md += ["", "## Findings (P1–P4)", ""] + header + [line(r) for r in rows if r["pattern"] != "P5"]
    md += ["", "## Integer per-update counters (P5)", "",
           "Counters change by a constant per call; most are event or state counters. Review only those that gate timing.", ""]
    md += header + [line(r) for r in rows if r["pattern"] == "P5"]
    (output / "report.md").write_text("\n".join(md) + "\n", encoding="utf-8")
    return data
