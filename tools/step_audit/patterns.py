"""Update-rate patterns over analyzed stores and compares (P1..P5 of the design)."""
from dataclasses import dataclass

from . import kdim

# Native functions absent from the generation: return exponents.
HLE_SUMMARIES = {
    "ActorStepTime": {"f1": kdim.exact(1, sources=frozenset({"ActorStepTime"}))},
    "Leaf_80032AA0": {"f1": kdim.exact(1, sources=frozenset({"elapsed playback 80032AA0"}))},
    "GameFrameWait": {},
}


@dataclass
class Finding:
    pattern: str
    function: str
    store: int | None
    field: object
    chain: tuple
    confidence: str
    covered_by: str | None
    ctx: tuple = ()
    detail: str = ""
    sources: frozenset = frozenset()


def _covered(prog, function):
    return prog.body_hooks.get(function) or prog.entry_hooks.get(function)


def find_patterns(prog) -> list:
    out = {}
    for (function, ctx), result in prog.results.items():
        cover = _covered(prog, function)
        for s in result.stores:
            if s.key is None or not s.prior_load_same_key:
                continue
            finding = None
            if not s.integer and s.delta_k == 2:
                finding = ("P2", "high", "increment carries the step twice")
            elif not s.integer and s.delta_kn is not None and s.delta_kn >= 2 and s.delta_unk:
                finding = ("P2", "medium", f"known step exponent {s.delta_kn} plus unresolved factors")
            elif not s.integer and s.delta_k == 0 and not s.value.const:
                finding = ("P1", "medium", "per-update increment without the step")
            elif not s.integer and s.factor_k == 0 and s.delta_kn is None:
                finding = ("P3", "medium", "per-update multiplicative factor without the step")
            elif s.integer and s.delta_k == 0 and not s.gated_by_bit:
                finding = ("P5", "medium", "integer counter changes by a constant per update")
            if finding:
                key = (finding[0], function, s.address)
                if key not in out or (ctx != (None,) * len(ctx) and out[key].confidence != "high"):
                    out[key] = Finding(finding[0], function, s.address, s.key, s.value.chain, finding[1], cover, ctx,
                                       finding[2], s.value.sources)
        for c in result.compares:
            for a, b in ((c.a, c.b), (c.b, c.a)):
                if a.k == 1 and b.const and b.k == 0:
                    key = ("P4", function, c.address)
                    out.setdefault(key, Finding("P4", function, None, None, a.chain, "low", cover, ctx,
                                                f"step-scaled value compared with a constant at {c.address:08X}"))
    return sorted(out.values(), key=lambda f: ({"high": 0, "medium": 1, "low": 2}[f.confidence], f.pattern, f.function))
