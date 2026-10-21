"""Step exponent of every value inside one function: value ∝ s^k when the update rate changes (s = step scale).

A value carries `kn` (the exponent of its known factors) and `unk` (it also has factors of unknown exponent).
The exact exponent `k` is kn when nothing is unknown; kn = None means the value mixes different exponents (⊥).
Persistent stores record the exponent of their increment relative to the same field (delta) and of a
multiplicative factor (factor); patterns.py judges them.
"""
from dataclasses import dataclass, field, replace

CLOCK_DT = {0x804D331C, 0x804D3320}          # +4 scaled dt, +8 raw dt (seconds)
CLOCK_CONST = {0x804D3334, 0x804D3340}       # +28 rate, +40 target
R2, R13 = 0x806CC700, 0x806CA620
SDATA2 = (0x806C0000, 0x806CC800)            # read-only constants addressed through r2 (approximate range)
K_LIMIT = 4
VOLATILE_I = [0] + list(range(3, 13))
VOLATILE_F = list(range(0, 14))
ARG_I, ARG_F = range(3, 11), range(1, 9)
RET = ("r3", "r4", "f1", "f2")
LOADS = {"lwz": "I", "lbz": "I", "lhz": "I", "lha": "I", "lfs": "F", "lfd": "F", "psq_l": "F",
         "lwzu": "I", "lbzu": "I", "lfsu": "F"}
STORES = {"stw", "stb", "sth", "stfs", "stfd", "psq_st", "stwu", "stbu", "stfsu", "stfdu"}
INDEXED_LOADS = {"lwzx", "lbzx", "lhzx", "lhax", "lfsx", "lfdx", "psq_lx", "lwzux", "lbzux", "lfsux"}
INDEXED_STORES = {"stwx", "stbx", "sthx", "stfsx", "stfdx", "psq_stx", "stwux", "stfsux"}
ADD = {"fadds", "fadd", "fsubs", "fsub", "ps_add", "ps_sub", "add", "subf"}
MUL = {"fmuls", "fmul", "ps_mul", "ps_muls0", "ps_muls1", "mullw"}
MADD = {"fmadds", "fmadd", "fmsubs", "fmsub", "fnmsubs", "fnmadds", "ps_madd", "ps_msub", "ps_madds0", "ps_madds1", "ps_nmadd"}
COPY = {"fneg", "fabs", "frsp", "fmr", "ps_neg", "ps_mr", "ps_abs", "extsh", "extsb", "neg", "srawi",
        "rlwinm", "xoris", "xori", "ori", "oris", "andi_", "andis_", "and", "andc", "nor", "xor", "slw", "srw", "sraw", "cntlzw"}


@dataclass(frozen=True)
class K:
    kn: int | None = None                    # exponent of the known factors; None: mixed exponents (⊥)
    unk: bool = True                         # has factors of unknown exponent
    sources: frozenset = frozenset()
    chain: tuple = ()
    fields: frozenset = frozenset()          # field keys loaded on the way
    pointer: tuple | None = None             # (base id, offset) when the value is an address
    delta: tuple = ()                        # ((field key, (kn, unk)), ...) increment relative to a field
    factor: tuple = ()                       # ((field key, (kn, unk)), ...) multiplicative factor of a field
    const: bool = False

    @property
    def k(self):
        return None if self.unk or self.kn is None else self.kn

    def at(self, address):
        return replace(self, chain=(self.chain + (address,))[-16:])


BOTTOM = K()                                  # nothing known: kn None
UNKNOWN = K(0, True)                          # one factor of unknown exponent


def exact(k, **kw):
    return K(k, False, **kw)


def _same(a: K, b: K) -> bool:
    """Equal in everything the analysis decides on (sources/chain are evidence only)."""
    return (a.kn == b.kn and a.unk == b.unk and a.pointer == b.pointer and a.const == b.const
            and a.delta == b.delta and a.factor == b.factor and a.fields == b.fields)


def merge(a: K, b: K) -> K:
    if a is b or _same(a, b):
        return a
    kn = a.kn if a.kn == b.kn else None
    return K(kn, a.unk or b.unk, a.sources | b.sources, a.chain if len(a.chain) >= len(b.chain) else b.chain,
             a.fields | b.fields, a.pointer if a.pointer == b.pointer else None,
             a.delta if a.delta == b.delta else tuple(sorted(set(a.delta) & set(b.delta), key=str)),
             a.factor if a.factor == b.factor else tuple(sorted(set(a.factor) & set(b.factor), key=str)),
             a.const and b.const)


def _add(a: K, b: K, address) -> K:
    if a.pointer and b.const and not b.pointer:
        return a.at(address)
    delta = dict(a.delta) | dict(b.delta)
    for x, y in ((a, b), (b, a)):
        for f in x.fields - y.fields:
            delta[f] = (y.kn, y.unk)
    if a.kn == b.kn:
        kn, unk = a.kn, a.unk or b.unk
    elif a.fields and not b.fields:
        kn, unk = a.kn, a.unk
    elif b.fields and not a.fields:
        kn, unk = b.kn, b.unk
    else:
        kn, unk = None, True
    return K(kn, unk, a.sources | b.sources, (a.chain + b.chain + (address,))[-16:], a.fields | b.fields, None,
             tuple(sorted(delta.items(), key=str)), (), a.const and b.const)


def _mul(a: K, b: K, address, divide=False) -> K:
    if a.kn is None or b.kn is None:
        kn = None
    else:
        kn = a.kn - b.kn if divide else a.kn + b.kn
        kn = kn if abs(kn) <= K_LIMIT else None
    factor = {}
    pairs = ((a, b),) if divide else ((a, b), (b, a))
    for x, y in pairs:
        for f in x.fields - y.fields:
            factor[f] = ((-y.kn if divide and y.kn is not None else y.kn), y.unk)
    return K(kn, a.unk or b.unk, a.sources | b.sources, (a.chain + b.chain + (address,))[-16:], a.fields | b.fields,
             None, (), tuple(sorted(factor.items(), key=str)), a.const and b.const)


@dataclass
class Store:
    address: int
    key: tuple | None
    value: K
    prior_load_same_key: bool
    delta_k: int | None
    delta_kn: int | None
    delta_unk: bool
    factor_k: int | None
    integer: bool
    gated_by_bit: bool = False


@dataclass
class Compare:
    address: int
    a: K
    b: K


@dataclass
class Call:
    address: int
    callee: str | None
    slot: int | None
    args: dict


@dataclass
class FunctionResult:
    ret_k: dict = field(default_factory=dict)
    stores: list = field(default_factory=list)
    compares: list = field(default_factory=list)
    calls: list = field(default_factory=list)
    reads: set = field(default_factory=set)
    unsupported: int = 0


@dataclass
class Env:
    summary: object = lambda callee, args: {}          # (callee hex | "hle:Name" | ("slot", n), {reg: K}) -> {ret reg: K}
    field_k: object = lambda key: None                 # raw field key -> K | None (None: unknown)
    arg_k: dict = field(default_factory=dict)          # {"f1": K, ...} at entry


class _State:
    def __init__(self, regs=None, stack=None, bit=False):
        self.regs = dict(regs or {})
        self.stack = dict(stack or {})
        self.bit = bit                                  # last condition came from a single-bit test

    def copy(self):
        return _State(self.regs, self.stack, self.bit)

    def join(self, other):
        changed = False
        for table, theirs in ((self.regs, other.regs), (self.stack, other.stack)):
            for key, b in theirs.items():
                a = table.get(key, BOTTOM)
                if a is b:
                    continue
                m = merge(a, b)
                if m is not a and not _same(m, a):
                    table[key] = m
                    changed = True
            for key in table.keys() - theirs.keys():
                a = table[key]
                if a is not BOTTOM and not _same(a, BOTTOM):
                    table[key] = merge(a, BOTTOM)
                    changed = True
        if other.bit and not self.bit:
            self.bit = True
            changed = True
        return changed


def _entry_state(env):
    s = _State()
    for n in range(32):
        s.regs["I", n] = exact(0, pointer=(("entry", n), 0))
        s.regs["F", n] = BOTTOM
    s.regs["I", 1] = exact(0, pointer=("stack", 0))
    s.regs["I", 2] = exact(0, pointer=("abs", R2), const=True)
    s.regs["I", 13] = exact(0, pointer=("abs", R13))
    for name, value in env.arg_k.items():
        kind, n = ("F" if name[0] == "f" else "I"), int(name[1:])
        s.regs[kind, n] = replace(value, pointer=value.pointer or s.regs[kind, n].pointer)
    return s


def _blocks(function):
    insns = function.insns
    starts = {0}
    index = {i.address: n for n, i in enumerate(insns)}
    for n, i in enumerate(insns):
        if i.address in function.labels:
            starts.add(n)
        if i.branch or i.returns:
            starts.add(n + 1)
    starts = sorted(s for s in starts if s < len(insns))
    blocks = list(zip(starts, starts[1:] + [len(insns)]))
    start_of = {a: idx for idx, (a, _) in enumerate(blocks)}
    successors = {}
    for idx, (a, b) in enumerate(blocks):
        last = insns[b - 1]
        succ = [start_of[index[t]] for t in last.branch if t in index and index[t] in start_of]
        if (not (last.branch or last.returns) or last.conditional) and idx + 1 < len(blocks):
            succ.append(idx + 1)
        successors[idx] = succ
    return blocks, successors


def _memory_key(state, base_reg, offset):
    """("abs", address), ("stack", offset) or (base id, offset)."""
    if base_reg == 0:
        return ("abs", offset & 0xFFFFFFFF)
    p = state.regs.get(("I", base_reg), BOTTOM).pointer
    if not p:
        return None
    if p[0] == "abs":
        return ("abs", (p[1] + offset) & 0xFFFFFFFF)
    return (p[0], p[1] + offset)


def _single_bit(op, args):
    if op == "rlwinm" and len(args) == 6 and args[5] is True:
        return args[3] == args[4]                       # mask of one bit
    if op in ("andi_", "andis_") and len(args) == 3:
        return args[2] and args[2] & (args[2] - 1) == 0
    return False


def _step(insn, s, env, result, record):
    op, args, roles = insn.op, insn.args, insn.roles
    if insn.conditional and insn.branch:
        return
    if op == "call":
        call_args = {f"r{n}": s.regs.get(("I", n), BOTTOM) for n in ARG_I}
        call_args.update({f"f{n}": s.regs.get(("F", n), BOTTOM) for n in ARG_F})
        target = insn.call or (("slot", insn.indirect_slot) if insn.indirect_slot is not None else None)
        ret = env.summary(target, call_args) if target else {}
        if record:   # fix proposals only need the step exponents of the FPR arguments
            result.calls.append(Call(insn.address, insn.call, insn.indirect_slot,
                                     {f"f{n}": call_args[f"f{n}"] for n in ARG_F if call_args[f"f{n}"].k is not None}))
        for n in VOLATILE_I:
            s.regs["I", n] = BOTTOM
        for n in VOLATILE_F:
            s.regs["F", n] = BOTTOM
        for name, value in ret.items():
            s.regs["F" if name[0] == "f" else "I", int(name[1:])] = value.at(insn.address)
        return
    if op in ("branch", "lmw", "stmw") or op in INDEXED_STORES:
        return
    if op in INDEXED_LOADS:
        s.regs["F" if op.startswith(("lf", "psq")) else "I", args[0]] = replace(UNKNOWN, chain=(insn.address,))
        return
    if op in ("fctiwz", "fctiw") and len(args) >= 2:
        s.regs["F", args[0]] = s.regs.get(("F", args[1]), BOTTOM).at(insn.address)
        return
    if op == "?" or not roles:
        result.unsupported += 1
        if args and isinstance(args[0], int):
            s.regs["F" if op.startswith(("f", "ps")) else "I", args[0]] = BOTTOM
        return
    if op in LOADS:
        kind = LOADS[op]
        dest, base, offset = args[0], args[1], args[2]
        key = _memory_key(s, base, offset)
        if key and key[0] == "abs" and key[1] in CLOCK_DT:
            value = exact(1, sources=frozenset({f"clock+{key[1] - 0x804D3318}"}), chain=(insn.address,))
        elif key and key[0] == "abs" and key[1] in CLOCK_CONST:
            value = exact(0, const=True, chain=(insn.address,))
        elif key and key[0] == "stack":
            value = s.stack.get(key[1], BOTTOM).at(insn.address)
        elif key and key[0] == "abs" and base == 2 and SDATA2[0] <= key[1] < SDATA2[1]:
            value = (env.field_k(key) or exact(0, const=True)).at(insn.address)
        else:
            known = env.field_k(key) if key else None
            value = replace(known or UNKNOWN, chain=(insn.address,), fields=frozenset({key}) if key else frozenset())
            if key and record:
                result.reads.add(key)
        if kind == "I" and op in ("lwz", "lwzu"):
            value = replace(value, pointer=(f"load@{insn.address:08X}", 0))
        s.regs[kind, dest] = value
        if op.endswith("u"):
            p = s.regs.get(("I", base), BOTTOM).pointer
            s.regs["I", base] = replace(s.regs.get(("I", base), BOTTOM), pointer=(p[0], p[1] + offset) if p else None)
        return
    if op in STORES:
        kind = "F" if roles[0] == "Fs" else "I"
        value = s.regs.get((kind, args[0]), BOTTOM)
        key = _memory_key(s, args[1], args[2])
        if key and key[0] == "stack":
            s.stack[key[1]] = value
        elif record and key and op != "stwu":
            delta = dict(value.delta).get(key)
            factor = dict(value.factor).get(key)
            result.stores.append(Store(
                insn.address, key, value, key in value.fields,
                None if delta is None or delta[1] or delta[0] is None else delta[0],
                None if delta is None else delta[0], True if delta is None else delta[1],
                None if factor is None or factor[1] or factor[0] is None else factor[0],
                kind == "I", s.bit))
        if op.endswith("u") and op != "stwu":
            p = s.regs.get(("I", args[1]), BOTTOM).pointer
            s.regs["I", args[1]] = replace(s.regs.get(("I", args[1]), BOTTOM), pointer=(p[0], p[1] + args[2]) if p else None)
        return
    if op in ("cmp", "cmpl", "cmpi", "cmpli", "fcmpo", "fcmpu"):
        s.bit = False
        if record:
            f = op.startswith("f")
            a = s.regs.get(("F" if f else "I", args[1]), BOTTOM)
            b = s.regs.get(("F", args[2]), BOTTOM) if f else (s.regs.get(("I", args[2]), BOTTOM) if op in ("cmp", "cmpl") else exact(0, const=True))
            if (a.k == 1 and b.const) or (b.k == 1 and a.const):   # only step-vs-constant compares are judged (P4)
                result.compares.append(Compare(insn.address, a, b))
        return
    if op in ("addi", "addis"):
        dest, src, imm = args
        amount = imm * (65536 if op == "addis" else 1)
        if src == 0:
            s.regs["I", dest] = exact(0, pointer=("abs", amount & 0xFFFFFFFF), const=True, chain=(insn.address,))
            return
        v = s.regs.get(("I", src), BOTTOM)
        value = _add(replace(v, pointer=None), exact(0, const=True), insn.address) if v.fields else v.at(insn.address)
        if v.pointer:
            base, off = v.pointer
            value = replace(value, pointer=(base, (off + amount) & 0xFFFFFFFF) if base == "abs" else (base, off + amount))
        s.regs["I", dest] = value
        return
    if op in ("addic", "addic_", "mulli", "subfic"):
        v = s.regs.get(("I", args[1]), BOTTOM)
        c = exact(0, const=True)
        s.regs["I", args[0]] = _mul(v, c, insn.address) if op == "mulli" else _add(v, c, insn.address)
        if op == "addic_":
            s.bit = False
        return
    if op == "or" and args[0] == args[2]:
        s.regs["I", args[1]] = s.regs.get(("I", args[0]), BOTTOM)
        return
    if op in ("mfspr", "mtspr") or op.startswith("cr"):
        if op == "mfspr":
            s.regs["I", args[0]] = BOTTOM
        return
    if _single_bit(op, args):
        s.bit = True
    srcs = [s.regs.get(("F" if r[0] == "F" else "I", a), BOTTOM) for r, a in zip(roles, args) if r in ("Fs", "Is")]
    dests = [("F" if r[0] == "F" else "I", a) for r, a in zip(roles, args) if r in ("Fd", "Id", "Iw")]
    if op in ADD and len(srcs) >= 2:
        value = _add(srcs[0], srcs[1], insn.address)
    elif op in MUL and len(srcs) >= 2:
        value = _mul(srcs[0], srcs[1], insn.address)
    elif op in ("fdivs", "fdiv", "divw", "divwu") and len(srcs) >= 2:
        value = _mul(srcs[0], srcs[1], insn.address, divide=True)
    elif op in MADD and len(srcs) >= 3:
        a, b, c = srcs[0], srcs[1], srcs[2]          # fmadds(t, a, b, c) = a*c + b
        value = _add(_mul(a, c, insn.address), b, insn.address)
    elif op == "fres" and srcs:
        value = _mul(exact(0, const=True), srcs[0], insn.address, divide=True)
    elif op in COPY and srcs:
        value = srcs[0].at(insn.address)
    elif op == "ps_sum0" and len(srcs) >= 3:
        value = _add(srcs[0], srcs[2], insn.address)
    elif op.startswith("ps_merge") and len(srcs) >= 2:
        value = merge(srcs[0], srcs[1])
    else:
        value = BOTTOM
        result.unsupported += 1
    for d in dests:
        s.regs[d] = value


def analyze(function, env: Env) -> FunctionResult:
    blocks, successors = _blocks(function)
    if not blocks:
        return FunctionResult()
    entry = {0: _entry_state(env)}
    work = [0]
    visits = {}
    scratch = FunctionResult()
    while work:
        idx = work.pop()
        visits[idx] = visits.get(idx, 0) + 1
        if visits[idx] > 64:
            continue
        s = entry[idx].copy()
        a, b = blocks[idx]
        for insn in function.insns[a:b]:
            _step(insn, s, env, scratch, False)
        for nxt in successors[idx]:
            if nxt not in entry:
                entry[nxt] = s.copy()
                work.append(nxt)
            elif entry[nxt].join(s):
                work.append(nxt)
    result = FunctionResult()
    rets = {}
    for idx, (a, b) in enumerate(blocks):
        if idx not in entry:
            continue
        s = entry[idx].copy()
        for insn in function.insns[a:b]:
            _step(insn, s, env, result, True)
            if insn.returns:
                for name in RET:
                    v = s.regs.get(("F" if name[0] == "f" else "I", int(name[1:])), BOTTOM)
                    rets[name] = merge(rets[name], v) if name in rets else v
    result.ret_k = rets
    return result
