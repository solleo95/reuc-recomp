"""Hybrid native versions of small leaf functions with branches (used by tools/leaf_hle.py when the straight-line generator rejects a function).

The registers live in a local ppc::Ctx (nothing else can alias it, so the compiler keeps them in machine registers); every register-only
instruction runs through the unchanged ppc::op_* function, so its semantics are exact by construction. Loads and stores with an unmodified base
register in RAM go straight to host memory (no MMIO test, one write-barrier stamp per base); everything else, calls excepted, runs through the
ordinary op on the local context. A function with a call, an unknown instruction or an unknown branch form is rejected. The translated body is
kept as the slow path, taken when a specialised base is not in RAM.
"""
import re

# Argument kinds per op: Id/Is int destination/source, Iw int read-modify-write, Ib base register (0 = literal zero), Fd/Fs float destination/source,
# Cd cr field written, Cb cr bit read/written, N number, B record bit, S spr number.
ROLES = {
    "addi": "Id Ib N", "addis": "Id Ib N", "add": "Id Is Is B", "subf": "Id Is Is B", "neg": "Id Is B", "mulli": "Id Is N", "mullw": "Id Is Is B",
    "subfic": "Id Is N", "or": "Is Id Is B", "and": "Is Id Is B", "xor": "Is Id Is B", "andc": "Is Id Is B", "nor": "Is Id Is B",
    "ori": "Is Id N", "oris": "Is Id N", "xori": "Is Id N", "xoris": "Is Id N", "andi_": "Is Id N", "andis_": "Is Id N",
    "extsb": "Is Id B", "extsh": "Is Id B", "cntlzw": "Is Id B", "slw": "Is Id Is B", "srw": "Is Id Is B", "sraw": "Is Id Is B", "srawi": "Is Id N B",
    "rlwinm": "Is Id N N N B", "rlwimi": "Is Iw N N N B", "rlwnm": "Is Id Is N N B",
    "cmp": "Cd Is Is", "cmpl": "Cd Is Is", "cmpi": "Cd Is N", "cmpli": "Cd Is N", "fcmpo": "Cd Fs Fs", "fcmpu": "Cd Fs Fs",
    "cror": "Cb Cb Cb", "crxor": "Cb Cb Cb", "crand": "Cb Cb Cb", "crnor": "Cb Cb Cb", "crandc": "Cb Cb Cb", "crorc": "Cb Cb Cb", "creqv": "Cb Cb Cb",
    "lwz": "Id Ib N", "lbz": "Id Ib N", "lhz": "Id Ib N", "lha": "Id Ib N", "stw": "Is Ib N", "stb": "Is Ib N", "sth": "Is Ib N",
    "lwzx": "Id Ib Is", "lbzx": "Id Ib Is", "lhzx": "Id Ib Is", "lhax": "Id Ib Is", "stwx": "Is Ib Is", "stbx": "Is Ib Is", "sthx": "Is Ib Is",
    "lfs": "Fd Ib N", "stfs": "Fs Ib N", "lfd": "Fd Ib N", "stfd": "Fs Ib N", "lfsx": "Fd Ib Is", "stfsx": "Fs Ib Is",
    "psq_l": "Fd Ib N N N", "psq_st": "Fs Ib N N N",
    "stwu": "Is Iw N", "lwzu": "Id Iw N", "stbu": "Is Iw N", "lbzu": "Id Iw N", "lfsu": "Fd Iw N", "stfsu": "Fs Iw N", "stfdu": "Fs Iw N",
    "fadds": "Fd Fs Fs B", "fsubs": "Fd Fs Fs B", "fmuls": "Fd Fs Fs B", "fdivs": "Fd Fs Fs B", "fadd": "Fd Fs Fs B", "fsub": "Fd Fs Fs B", "fmul": "Fd Fs Fs B",
    "fmadds": "Fd Fs Fs Fs B", "fmsubs": "Fd Fs Fs Fs B", "fnmsubs": "Fd Fs Fs Fs B", "fnmadds": "Fd Fs Fs Fs B", "fmadd": "Fd Fs Fs Fs B", "fmsub": "Fd Fs Fs Fs B",
    "fneg": "Fd Fs B", "fmr": "Fd Fs B", "fabs": "Fd Fs B", "frsp": "Fd Fs B", "fres": "Fd Fs B", "fsel": "Fd Fs Fs Fs B",
    "ps_add": "Fd Fs Fs B", "ps_sub": "Fd Fs Fs B", "ps_mul": "Fd Fs Fs B", "ps_muls0": "Fd Fs Fs B", "ps_muls1": "Fd Fs Fs B",
    "ps_madd": "Fd Fs Fs Fs B", "ps_msub": "Fd Fs Fs Fs B", "ps_madds0": "Fd Fs Fs Fs B", "ps_madds1": "Fd Fs Fs Fs B", "ps_nmadd": "Fd Fs Fs Fs B",
    "ps_merge00": "Fd Fs Fs B", "ps_merge01": "Fd Fs Fs B", "ps_merge10": "Fd Fs Fs B", "ps_merge11": "Fd Fs Fs B",
    "ps_neg": "Fd Fs B", "ps_mr": "Fd Fs B", "ps_abs": "Fd Fs B", "ps_sum0": "Fd Fs Fs Fs B", "ps_sum1": "Fd Fs Fs Fs B",
    "mfspr": "Id S", "mtspr": "Is S",
}
SPEC_LOAD = {"lwz", "lbz", "lhz", "lha", "lfs"}
SPEC_STORE = {"stw", "stb", "sth", "stfs"}
XER_WRITE = {"srawi", "sraw", "subfic"}
OP = re.compile(r"ppc::op_(\w+)\(c,\s*([^)]*)\)")
BRANCH = re.compile(r"\{ \{ if \((?P<cond>.*)\) \{ (?P<poll>ppc::poll\(c\); )?goto (?P<lbl>L_[0-9A-F]+); \} \} \}")
GOTO = re.compile(r"\{ (?P<poll>ppc::poll\(c\); )?goto (?P<lbl>L_[0-9A-F]+); \}")


def parse_hybrid(body):
    items, raw = [], []
    for line in body.split("\n"):
        s = line.strip()
        if not s or s.startswith("UC_ENTER"):
            continue
        s = re.sub(r"^/\*[0-9A-F]+\*/\s*", "", s)
        if s.startswith("ppc::trap"):
            raw.append(s)
            continue
        if re.fullmatch(r"L_[0-9A-F]+:;", s):
            items.append(("label", s[:-2]))
            raw.append(s)
            continue
        raw.append(s)
        if re.fullmatch(r"\{ /\*.*\*/ \}", s):
            continue
        if re.search(r"fn_[0-9A-F]+\(c\)|hle_\w+\(c\)|call_indirect", s):
            return None, "calls another function"
        if s.rstrip() in ("{ return; }", "{ return;"):
            items.append(("return",))
            continue
        m = BRANCH.fullmatch(s)
        if m:
            items.append(("branch", m.group("cond"), m.group("lbl"), bool(m.group("poll"))))
            continue
        m = GOTO.fullmatch(s)
        if m:
            items.append(("goto", m.group("lbl"), bool(m.group("poll"))))
            continue
        m = OP.search(s)
        if m and s.startswith("{ ppc::op_") or (m and s.startswith("ppc::op_")):
            name, args = m.group(1), [a.strip() for a in m.group(2).split(",")]
            if name not in ROLES:
                return None, "unsupported op " + name
            if len(args) != len(ROLES[name].split()):
                return None, "unexpected argument count of " + name
            items.append(("op", name, args))
            continue
        return None, "unparsed line: " + s
    return (items, raw), None


def analyse(items):
    ints, ints_w, fl, fl_w, crw, crr = set(), set(), set(), set(), set(), set()
    base_ops = {}      # base register -> list of (op, offset)
    xer, xer_w, lr_w, ctr_w, gqr = False, False, False, False, set()
    for it in items:
        if it[0] == "branch":
            for n in re.findall(r"crbit\(c, (\d+)\)", it[1]):
                crr.add(int(n) >> 2); xer = xer
            for n in re.findall(r"c\.r\[(\d+)\]", it[1]):
                ints.add(int(n))
            continue
        if it[0] != "op":
            continue
        name, args = it[1], it[2]
        roles = ROLES[name].split()
        vals = [int(a) if re.fullmatch(r"-?\d+", a) else a for a in args]
        for kind, v in zip(roles, vals):
            if kind in ("Is", "Iw"): ints.add(v)
            if kind in ("Id", "Iw"): ints.add(v); ints_w.add(v)
            if kind == "Ib" and v != 0: ints.add(v)
            if kind == "Fs": fl.add(v)
            if kind == "Fd": fl.add(v); fl_w.add(v)
            if kind == "Cd": crw.add(v); xer = True
            if kind == "Cb": crr.add(v >> 2); crw.add(v >> 2)
            if kind == "B" and v == "true": crw.add(0); xer = True
        if name in ("andi_", "andis_"): crw.add(0); xer = True
        if name in XER_WRITE: xer = True; xer_w = True
        if name == "mfspr" and vals[1] == 8 or name == "mtspr" and vals[1] == 8:
            lr_w = lr_w or name == "mtspr"
        if name in ("mfspr", "mtspr") and vals[1] not in (8, 9):
            raise ValueError("spr %s" % vals[1])
        if name == "mtspr" and vals[1] == 9: ctr_w = True
        if name in ("psq_l", "psq_st"): gqr.add(vals[4])
        if name in ("fcmpo", "fcmpu"): xer = xer
        if name in SPEC_LOAD | SPEC_STORE and ROLES[name].split()[1] == "Ib" and isinstance(vals[1], int) and vals[1] not in (0, 1, 2, 13):
            size = {"lwz": 4, "stw": 4, "lfs": 4, "stfs": 4, "lhz": 2, "lha": 2, "sth": 2, "lbz": 1, "stb": 1}[name]
            base_ops.setdefault(vals[1], []).append((name, vals[2], size))
    return dict(ints=ints, ints_w=ints_w, fl=fl, fl_w=fl_w, crw=crw, crr=crr, base_ops=base_ops, xer=xer, xer_w=xer_w, lr_w=lr_w, ctr_w=ctr_w, gqr=gqr)


def to_lc(text):
    return re.sub(r"\bc\b", "lc", text)


def emit_hybrid(address, name, items, raw):
    a = analyse(items)
    specialised = {b: ops for b, ops in a["base_ops"].items() if b not in a["ints_w"]}
    L = []
    L.append("namespace {")
    L.append("void slow_%s(ppc::Ctx& c) {" % name)
    for r in raw:
        L.append("    " + r)
    L.append("}")
    L.append("}  // namespace")
    L.append("static void fast_%s(ppc::Ctx& c) {   // fn_%s" % (name, address))
    cond = []
    for b, ops in sorted(specialised.items()):
        lo = min(o[1] for o in ops); hi = max(o[1] + o[2] for o in ops)
        cond.append("in_ram(c.r[%d] + uint32_t(%d), %d)" % (b, lo, hi - lo))
    if any(n in ("psq_l", "psq_st") for n in [it[1] for it in items if it[0] == "op"]):
        cond.append("float_gqr0(c)")
    L.append("    if (leaf_off(0x%s)%s) { slow_%s(c); return; }" % (address, (" || !(%s)" % " && ".join(cond)) if cond else "", name))
    L.append("    ppc::Ctx lc; lc.mem = c.mem;")
    for b, ops in sorted(specialised.items()):
        L.append("    uint8_t* const P%d = ppc::hp(c, c.r[%d]); bool stamped%d = false;" % (b, b, b))
    copy_in, copy_out, flush = [], [], []
    for r in sorted(a["ints"]):
        copy_in.append("lc.r[%d] = c.r[%d];" % (r, r)); flush.append("c.r[%d] = lc.r[%d];" % (r, r))
        if r == 0 or 3 <= r <= 12 and r in a["ints_w"]: copy_out.append("c.r[%d] = lc.r[%d];" % (r, r))
    for r in sorted(a["fl"]):
        copy_in.append("lc.f[%d] = c.f[%d]; lc.ps1[%d] = c.ps1[%d];" % (r, r, r, r)); flush.append("c.f[%d] = lc.f[%d]; c.ps1[%d] = lc.ps1[%d];" % (r, r, r, r))
        if r in a["fl_w"] and r <= 13: copy_out.append("c.f[%d] = lc.f[%d]; c.ps1[%d] = lc.ps1[%d];" % (r, r, r, r))
    for f in sorted(a["crr"] | a["crw"]):
        copy_in.append("lc.cr[%d] = c.cr[%d];" % (f, f)); flush.append("c.cr[%d] = lc.cr[%d];" % (f, f))
        if f in a["crw"]: copy_out.append("c.cr[%d] = lc.cr[%d];" % (f, f))
    if a["xer"]:
        copy_in.append("lc.xer_so = c.xer_so; lc.xer_ca = c.xer_ca; lc.xer_ov = c.xer_ov;"); flush.append("c.xer_so = lc.xer_so; c.xer_ca = lc.xer_ca; c.xer_ov = lc.xer_ov;")
        if a["xer_w"]: copy_out.append("c.xer_ca = lc.xer_ca;")
    uses_lr = any(it[0] == "op" and it[1] in ("mfspr", "mtspr") and it[2][1] == "8" for it in items)
    uses_ctr = any(it[0] == "op" and it[1] in ("mfspr", "mtspr") and it[2][1] == "9" for it in items)
    if uses_lr:
        copy_in.append("lc.lr = c.lr;"); flush.append("c.lr = lc.lr;"); copy_out.append("c.lr = lc.lr;")
    if uses_ctr:
        copy_in.append("lc.ctr = c.ctr;"); flush.append("c.ctr = lc.ctr;"); copy_out.append("c.ctr = lc.ctr;")
    for g in sorted(a["gqr"]):
        copy_in.append("lc.gqr[%d] = c.gqr[%d];" % (g, g))
    if any((it[0] == "branch" and it[3]) or (it[0] == "goto" and it[2]) for it in items):
        copy_out = list(flush)   # a loop polls: the registers flushed at a safe point must end at their final values too (the stack pointer, below all)
    for line in copy_in:
        L.append("    " + line)
    for it in items:
        if it[0] == "label":
            L.append(it[1] + ":;")
        elif it[0] == "return":
            L.append("    goto L_done;")
        elif it[0] == "goto":
            if it[2]:
                L.append("    if ((++c.poll_count & 0x7FFu) == 0) { %s ppc::poll_slow(c); %s }" % (" ".join(flush), " ".join(copy_in)))
            L.append("    goto %s;" % it[1])
        elif it[0] == "branch":
            if it[3]:
                L.append("    if (%s) { if ((++c.poll_count & 0x7FFu) == 0) { %s ppc::poll_slow(c); %s } goto %s; }" % (to_lc(it[1]), " ".join(flush), " ".join(copy_in), it[2]))
            else:
                L.append("    if (%s) goto %s;" % (to_lc(it[1]), it[2]))
        else:
            name_, args = it[1], it[2]
            roles = ROLES[name_].split()
            vals = [int(x) if re.fullmatch(r"-?\d+", x) else x for x in args]
            base = vals[1] if len(vals) > 1 and roles[1] == "Ib" else None
            if name_ in SPEC_LOAD | SPEC_STORE and base in specialised:
                d = vals[2]; P = "(P%d + (%d))" % (base, d)
                if name_ == "lwz": L.append("    lc.r[%d] = ld32(%s);" % (vals[0], P))
                elif name_ == "lbz": L.append("    lc.r[%d] = *%s;" % (vals[0], P))
                elif name_ == "lhz": L.append("    lc.r[%d] = ld16(%s);" % (vals[0], P))
                elif name_ == "lha": L.append("    lc.r[%d] = uint32_t(int32_t(int16_t(ld16(%s))));" % (vals[0], P))
                elif name_ == "lfs": L.append("    { const double v = ldf(%s); lc.f[%d] = v; lc.ps1[%d] = v; }" % (P, vals[0], vals[0]))
                else:
                    ops = specialised[base]
                    slo = min(o[1] for o in ops if o[0] in SPEC_STORE); shi = max(o[1] + o[2] for o in ops if o[0] in SPEC_STORE)
                    L.append("    if (!stamped%d) { gx::note_write_range(c.r[%d] + uint32_t(%d), %d); stamped%d = true; }" % (base, base, slo, shi - slo, base))
                    if name_ == "stw": L.append("    st32(%s, lc.r[%d]);" % (P, vals[0]))
                    elif name_ == "stb": L.append("    *%s = uint8_t(lc.r[%d]);" % (P, vals[0]))
                    elif name_ == "sth": L.append("    st16(%s, lc.r[%d]);" % (P, vals[0]))
                    else: L.append("    stf(%s, lc.f[%d]);" % (P, vals[0]))
            else:
                L.append("    ppc::op_%s(lc, %s);" % (name_, ", ".join(args)))
    L.append("L_done:;")
    for line in copy_out:
        L.append("    " + line)
    L.append("}")
    L.append("void hle_%s(ppc::Ctx& c) {" % name)
    L.append("    if (!leaf_verify(0x%s)) { fast_%s(c); return; }" % (address, name))
    L.append("    static ppc::Ctx shadow; shadow = c; fast_%s(c); slow_%s(shadow); verify_compare(\"Leaf_%s\", shadow, c);" % (name, name, address))
    L.append("}")

    return "\n".join(L)


def sda_offsets(items):
    """Offsets from r2/r13 that the function reads (the test fills them with random data)."""
    out = []
    for it in items:
        if it[0] != "op":
            continue
        roles = ROLES[it[1]].split()
        vals = [int(a) if re.fullmatch(r"-?\d+", a) else a for a in it[2]]
        if len(roles) > 2 and roles[1] == "Ib" and vals[1] in (2, 13) and roles[2] == "N":
            for off in range(vals[2], vals[2] + 8, 4):
                if off not in out:
                    out.append(off)
    return out


def has_loop(items):
    """True when a branch or goto jumps back to a label already passed (the translated loop; such a function may never end on random inputs)."""
    seen = set()
    for it in items:
        if it[0] == "label":
            seen.add(it[1])
        elif it[0] == "branch" and it[2] in seen or it[0] == "goto" and it[1] in seen:
            return True
    return False
