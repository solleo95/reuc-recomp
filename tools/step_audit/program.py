"""Whole-program step-exponent analysis: function summaries per argument context, field types to a fixpoint."""
from dataclasses import dataclass, field
import sys

from . import kdim

ARG_CTX = ("f1", "f2", "f3", "f4")
ROUNDS = 3
MAX_DEPTH = 48          # call-summary nesting; deeper callees are ⊥ (Python recursion bound)


@dataclass
class Program:
    functions: dict
    classes: dict = field(default_factory=dict)        # function -> vtable when the function belongs to exactly one class
    field_k: dict = field(default_factory=dict)        # normalized field key -> exact k or None (⊥)
    results: dict = field(default_factory=dict)        # (function, ctx) -> FunctionResult with normalized keys
    body_hooks: dict = field(default_factory=dict)     # retained body entry -> hook name
    entry_hooks: dict = field(default_factory=dict)    # hooked entry -> hook name


def _classes(functions, index):
    owners = {}
    for vt, slots in index.vtables.items():
        for entry in slots:
            if entry:
                owners.setdefault(entry, set()).add(vt)
    return {f: next(iter(v)) for f, v in owners.items() if len(v) == 1}


def normalize(key, function, classes):
    if key is None or key[0] in ("abs", "stack"):
        return key
    base, offset = key
    if base == ("entry", 3) and function in classes:
        return (("class", classes[function]), offset)
    return ((function, base), offset)


class _Analyzer:
    def __init__(self, prog, index, hle_summaries, body_of_hook):
        self.prog, self.index, self.hle, self.body_of_hook = prog, index, hle_summaries, body_of_hook
        self.memo, self.active = {}, set()
        self.field_types = {}
        self.pointer_types = {}
        self.stack = []

    def receiver_class(self, receiver):
        """Class of a virtual call's r3: the caller's own `this`, or the pointee type of the field it was loaded from."""
        if receiver is None or not self.stack:
            return None
        caller = self.stack[-1]
        if receiver.pointer == (("entry", 3), 0):
            return self.prog.classes.get(caller)
        if len(receiver.fields) == 1:
            return self.pointer_types.get(normalize(next(iter(receiver.fields)), caller, self.prog.classes))
        return None

    def field_k(self, function):
        def lookup(key):
            k = self.field_types.get(normalize(key, function, self.prog.classes))
            return kdim.exact(k) if k is not None else None
        return lookup

    def target(self, callee):
        if callee in self.prog.entry_hooks:
            body = self.body_of_hook.get(self.prog.entry_hooks[callee])
            return body if body in self.prog.functions else None
        if isinstance(callee, str) and callee.startswith("hle:"):
            body = self.body_of_hook.get(callee[4:])
            return body if body in self.prog.functions else None
        return callee if callee in self.prog.functions else None

    def summary(self, callee, args):
        if isinstance(callee, tuple):
            cls = self.receiver_class(args.get("r3"))
            slots = self.index.vtables.get(cls, []) if cls is not None else []
            i = callee[1] // 4
            targets = {slots[i]} if i < len(slots) and slots[i] else self.index.slot_targets(callee[1])
            rets = [self.summary(t, args) for t in sorted(targets)]
            if not rets:
                return {}
            out = {}
            for name in kdim.RET:
                vals = [r.get(name, kdim.BOTTOM) for r in rets]
                m = vals[0]
                for v in vals[1:]:
                    m = kdim.merge(m, v)
                out[name] = m
            return out
        name = callee[4:] if isinstance(callee, str) and callee.startswith("hle:") else self.prog.entry_hooks.get(callee)
        if name in self.hle:
            return self.hle[name]
        if callee in self.hle:
            return self.hle[callee]
        f = self.target(callee)
        if f is None or len(self.active) >= MAX_DEPTH:
            return {}
        ctx = tuple(args.get(a, kdim.BOTTOM).k for a in ARG_CTX)
        return self.run(f, ctx).ret_k

    def run(self, function, ctx):
        key = (function, ctx)
        if key in self.memo:
            return self.memo[key]
        if key in self.active:
            return kdim.FunctionResult()
        self.active.add(key)
        self.stack.append(function)
        arg_k = {a: kdim.exact(k, sources=frozenset({f"arg:{a}"})) for a, k in zip(ARG_CTX, ctx) if k is not None}
        env = kdim.Env(summary=self.summary, field_k=self.field_k(function), arg_k=arg_k)
        try:
            result = kdim.analyze(self.prog.functions[function], env)
        finally:
            self.stack.pop()
        for s in result.stores:
            s.key = normalize(s.key, function, self.prog.classes)
        result.reads = {normalize(r, function, self.prog.classes) for r in result.reads}
        self.active.discard(key)
        self.memo[key] = result
        return result


def _static_types(results):
    seen = {}
    for result in results.values():
        for s in result.stores:
            if s.key is None or s.key[0] == "abs" and False:
                continue
            if s.prior_load_same_key:
                seen.setdefault(s.key, set()).add("acc")
            else:
                seen.setdefault(s.key, set()).add(s.value.k)
    return {key: next(iter(ks)) for key, ks in seen.items() if len(ks) == 1 and None not in ks and "acc" not in ks}


def run_program(functions, dump_index, hle_summaries, empirical=None, body_hooks=None, entry_hooks=None, progress=False,
                pointer_types=None):
    prog = Program(functions, _classes(functions, dump_index), body_hooks=dict(body_hooks or {}),
                   entry_hooks=dict(entry_hooks or {}))
    prog.dump_index = dump_index
    body_of_hook = {name: body for body, name in prog.body_hooks.items()}
    empirical = dict(empirical or {})
    types = dict(empirical)
    for round_no in range(ROUNDS):
        analyzer = _Analyzer(prog, dump_index, hle_summaries, body_of_hook)
        analyzer.field_types = types
        analyzer.pointer_types = dict(pointer_types or {})
        for n, f in enumerate(sorted(functions)):
            analyzer.run(f, (None,) * len(ARG_CTX))
            if progress and n % 2000 == 0:
                print(f"round {round_no + 1}: {n}/{len(functions)}", file=sys.stderr, flush=True)
        static = _static_types(analyzer.memo)
        new = dict(empirical)
        for key, k in static.items():
            if key in empirical and empirical[key] != k:
                new.pop(key, None)              # static and measured disagree: unknown
            else:
                new[key] = k
        prog.results = analyzer.memo
        if new == types:
            break
        types = new
    prog.field_k = types
    return prog
