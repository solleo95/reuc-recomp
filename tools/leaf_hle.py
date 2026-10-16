"""Native versions of small straight-line leaf functions of the translated game.

python -I tools/leaf_hle.py <generated dir> <hle address>...

For every function it writes (runtime/hle/leaf_gen.cpp, tests/test_leaf_hle.cpp, and the project/hle_map.txt lines on stdout):
  * a native function that runs the same instructions on local registers and host memory: loads and stores in the original order, the original
    single-precision rounding, the stack frame (register spills, the link register slot) kept in a local array, the write barrier stamped once per
    touched span instead of once per store. It is used only when every base register points into RAM and the GQRs are float formats; otherwise it
    runs the original instruction sequence unchanged (the slow path is a verbatim copy of the translated body).
  * a differential test: the translated instructions against the native function on randomised memory and registers; memory, every register,
    the link register and the paired-single halves must match.
Only straight-line leaves (no branch, no call) made of the supported instructions are accepted; anything else is rejected with its reason.
"""
import re
import sys
from pathlib import Path

SUPPORTED_INT = {"addi", "addis", "or", "lwz", "stw", "stwu", "mfspr", "mtspr"}
SUPPORTED_FLOAT = {"lfs", "stfs", "lfd", "stfd", "psq_l", "psq_st", "fadds", "fsubs", "fmuls", "fmadds", "fmsubs", "fneg", "fmr", "fabs"}
OP = re.compile(r"ppc::op_(\w+)\(c,\s*([^)]*)\)")


def load_function(gen_dir, address):
    for path in sorted(gen_dir.glob("game_*.cpp")):
        text = path.read_text(errors="replace")
        m = re.search(r"^void fn_%s\(ppc::Ctx& c\) \{\n(.*?)^\}\n" % address, text, re.S | re.M)
        if m:
            return m.group(1)
    raise SystemExit("function %s not found in %s" % (address, gen_dir))


def parse(body):
    ops, raw = [], []
    for line in body.split("\n"):
        s = line.strip()
        if not s or s.startswith("UC_ENTER") or re.match(r"^L_[0-9A-F]+:;", s) or s.startswith("ppc::trap"):
            continue
        if re.search(r"goto|crbit|fn_[0-9A-F]+\(c\)|hle_|call_indirect|poll\(", s):
            return None, "branch or call"
        if re.search(r"\{\s*return;", s):
            continue
        m = OP.search(s)
        if not m:
            return None, "unparsed line: " + s
        args = [a.strip() for a in m.group(2).split(",")]
        ops.append((m.group(1), args))
        raw.append(re.sub(r"^/\*[0-9A-F]+\*/\s*\{\s*", "", s).rstrip(" }").rstrip() + (";" if not s.rstrip(" }").endswith(";") else ""))
    for name, args in ops:
        if name not in SUPPORTED_INT | SUPPORTED_FLOAT:
            return None, "unsupported op " + name
        if name == "or" and args[-1] != "false":
            return None, "or with record bit"
    return (ops, raw), None


def analyse(ops):
    """Registers read before written, registers written, memory bases with their offset spans, frame size."""
    int_read, int_written, f_read, f_written, p1_written = set(), set(), set(), set(), set()
    spans = {}   # base register -> [min, max_end, store_min, store_max_end]
    frame = 0

    def read_int(r):
        if r not in int_written and r != 0:
            int_read.add(r)
        elif r == 0 and 0 not in int_written:
            int_read.add(0)

    def mem(base, off, size, store):
        s = spans.setdefault(base, [10**9, -10**9, 10**9, -10**9])
        s[0] = min(s[0], off); s[1] = max(s[1], off + size)
        if store:
            s[2] = min(s[2], off); s[3] = max(s[3], off + size)

    for name, a in ops:
        n = [int(x) if re.fullmatch(r"-?\d+", x) else x for x in a]
        if name == "addi" or name == "addis":
            if n[1] != 0: read_int(n[1])
            int_written.add(n[0])
        elif name == "or":
            read_int(n[0]); read_int(n[2]); int_written.add(n[1])
        elif name == "lwz":
            if n[1] != 1: read_int(n[1])
            mem(n[1], n[2], 4, False); int_written.add(n[0])
        elif name == "stw":
            read_int(n[0])
            if n[1] != 1: read_int(n[1])
            mem(n[1], n[2], 4, True)
        elif name == "stwu":
            frame = -n[2]
            read_int(n[0])
        elif name == "mfspr":
            int_written.add(n[0])
        elif name == "mtspr":
            read_int(n[0])
        elif name in ("lfs", "lfd"):
            if n[1] != 1: read_int(n[1])
            mem(n[1], n[2], 4 if name == "lfs" else 8, False); f_written.add(n[0]); p1_written.add(n[0]) if name == "lfs" else None
        elif name in ("stfs", "stfd"):
            if n[0] not in f_written: f_read.add(n[0])
            if n[1] != 1: read_int(n[1])
            mem(n[1], n[2], 4 if name == "stfs" else 8, True)
        elif name == "psq_l":
            if n[1] != 1: read_int(n[1])
            mem(n[1], n[2], 4 if n[3] else 8, False); f_written.add(n[0]); p1_written.add(n[0])
        elif name == "psq_st":
            if n[0] not in f_written: f_read.add(n[0])
            if n[0] not in p1_written: f_read.add(("p1", n[0]))
            if n[1] != 1: read_int(n[1])
            mem(n[1], n[2], 4 if n[3] else 8, True)
        elif name in ("fadds", "fsubs"):
            for r in (n[1], n[2]):
                if r not in f_written: f_read.add(r)
            f_written.add(n[0]); p1_written.add(n[0])
        elif name == "fmuls":
            for r in (n[1], n[2]):
                if r not in f_written: f_read.add(r)
            f_written.add(n[0]); p1_written.add(n[0])
        elif name in ("fmadds", "fmsubs"):
            for r in (n[1], n[2], n[3]):
                if r not in f_written: f_read.add(r)
            f_written.add(n[0]); p1_written.add(n[0])
        elif name in ("fneg", "fmr", "fabs"):
            if n[1] not in f_written: f_read.add(n[1])
            f_written.add(n[0])
    return int_read, int_written, f_read, f_written, p1_written, spans, frame


MEMORY_OPS = {"lwz", "stw", "lfs", "lfd", "stfs", "stfd", "psq_l", "psq_st"}


def base_of(op, a):
    return int(a[0]) if False else int(a[1]) if op in ("lwz", "stw", "lfs", "lfd", "stfs", "stfd", "psq_l", "psq_st") else None


def off_of(op, a):
    return int(a[2])


def emit_native(address, name, ops, raw):
    int_read, int_written, f_read, f_written, p1_written, spans, frame = analyse(ops)
    flat_f = {x for x in f_read if isinstance(x, int)}
    p1_read = {x[1] for x in f_read if isinstance(x, tuple)}
    ram_bases = sorted(b for b in spans if b != 1)
    L = []
    L.append("void hle_%s(ppc::Ctx& c) {   // fn_%s" % (name, address))
    cond = []
    for b in ram_bases:
        lo, hi = spans[b][0], spans[b][1]
        cond.append("in_ram(c.r[%d] + uint32_t(%d), %d)" % (b, lo, hi - lo))
    uses_psq = any(op in ("psq_l", "psq_st") for op, _ in ops)
    if uses_psq:
        cond.append("float_gqr0(c)")
    L.append("    if (!(%s)) {   // the original instruction sequence" % (" && ".join(cond) if cond else "true"))
    for r in raw:
        L.append("        ppc::" + r.replace("ppc::", ""))
    L.append("        return;")
    L.append("    }")
    for b in ram_bases:
        L.append("    uint8_t* const P%d = ppc::hp(c, c.r[%d]);" % (b, b))
    if any(base_of(op, a_) == 1 and off_of(op, a_) >= frame for op, a_ in ops if op in MEMORY_OPS):
        L.append("    uint8_t* const P1c = ppc::hp(c, c.r[1]);   // the caller's frame: stack arguments and the link-register slot")
    if frame:
        L.append("    uint8_t sp[%d] = {};   // the stack frame: spills and the link-register slot are dead after the return" % (frame + 32))
    for b in ram_bases:
        s = spans[b]
        if s[3] > s[2]:
            L.append("    gx::note_write_range(c.r[%d] + uint32_t(%d), %d);" % (b, s[2], s[3] - s[2]))
    for r in sorted(int_read):
        L.append("    uint32_t r%d = c.r[%d];" % (r, r))
    for r in sorted(flat_f):
        L.append("    double f%d = c.f[%d];" % (r, r))
    for r in sorted(p1_read):
        L.append("    double p%d = c.ps1[%d];" % (r, r))
    declared_i = set(int_read)
    declared_f = set(flat_f)
    declared_p = set(p1_read)
    written_p = set()

    def di(r):
        if r not in declared_i:
            declared_i.add(r); return "uint32_t "
        return ""

    def df(r):
        if r not in declared_f:
            declared_f.add(r); return "double "
        return ""

    def dp(r):
        if r not in declared_p:
            declared_p.add(r); return "double "
        return ""

    def addr(base, off, size):
        return "(sp + %d)" % off if base == 1 else "(P%d + %d)" % (base, off - 0) if False else None

    def ptr(base, off):
        if base == 1:   # below the frame size: the function's own frame (a local array); above it: the caller's frame (stack arguments, the link slot), real memory
            if off < frame:
                return "(sp + %d)" % off
            return "(P1c + %d)" % (off - frame)
        return "(P%d + (%d))" % (base, off)

    for name_, a in ops:
        n = [int(x) if re.fullmatch(r"-?\d+", x) else x for x in a]
        if name_ == "addi":
            src = "0u" if n[1] == 0 else "r%d" % n[1]
            if n[1] == 1: src = "0u"   # only used for the epilogue addi r1,r1,N: r1 is not modelled
            if n[0] == 1: continue
            L.append("    %sr%d = %s + uint32_t(%d);" % (di(n[0]), n[0], src, n[2]))
        elif name_ == "addis":
            src = "0u" if n[1] == 0 else "r%d" % n[1]
            L.append("    %sr%d = %s + (uint32_t(%d) << 16);" % (di(n[0]), n[0], src, n[2]))
        elif name_ == "or":
            L.append("    %sr%d = r%d | r%d;" % (di(n[1]), n[1], n[0], n[2]))
        elif name_ == "lwz":
            if n[0] == 1: continue
            L.append("    %sr%d = ld32(%s);" % (di(n[0]), n[0], ptr(n[1], n[2])))
        elif name_ == "stw":
            L.append("    st32(%s, r%d);" % (ptr(n[1], n[2]), n[0]))
        elif name_ == "stwu":
            L.append("    st32(sp + 0, c.r[1]);")
        elif name_ == "mfspr":
            L.append("    %sr%d = c.lr;" % (di(n[0]), n[0]))
        elif name_ == "mtspr":
            L.append("    c.lr = r%d;" % n[0])
        elif name_ == "lfs":
            L.append("    %sf%d = ldf(%s); %sp%d = f%d;" % (df(n[0]), n[0], ptr(n[1], n[2]), dp(n[0]), n[0], n[0])); written_p.add(n[0])
        elif name_ == "lfd":
            L.append("    %sf%d = ldd(%s);" % (df(n[0]), n[0], ptr(n[1], n[2])))
        elif name_ == "stfs":
            L.append("    stf(%s, f%d);" % (ptr(n[1], n[2]), n[0]))
        elif name_ == "stfd":
            L.append("    std_(%s, f%d);" % (ptr(n[1], n[2]), n[0]))
        elif name_ == "psq_l":
            if n[3]:
                L.append("    %sf%d = ldf(%s); %sp%d = 1.0;" % (df(n[0]), n[0], ptr(n[1], n[2]), dp(n[0]), n[0]))
            else:
                L.append("    %sf%d = ldf(%s); %sp%d = ldf(%s);" % (df(n[0]), n[0], ptr(n[1], n[2]), dp(n[0]), n[0], ptr(n[1], n[2] + 4)))
            written_p.add(n[0])
        elif name_ == "psq_st":
            L.append("    stf(%s, f%d);" % (ptr(n[1], n[2]), n[0]))
            if not n[3]:
                L.append("    stf(%s, p%d);" % (ptr(n[1], n[2] + 4), n[0]))
        elif name_ == "fadds":
            L.append("    %sf%d = rs(f%d + f%d); %sp%d = f%d;" % (df(n[0]), n[0], n[1], n[2], dp(n[0]), n[0], n[0])); written_p.add(n[0])
        elif name_ == "fsubs":
            L.append("    %sf%d = rs(f%d - f%d); %sp%d = f%d;" % (df(n[0]), n[0], n[1], n[2], dp(n[0]), n[0], n[0])); written_p.add(n[0])
        elif name_ == "fmuls":
            L.append("    %sf%d = rs(f%d * f%d); %sp%d = f%d;" % (df(n[0]), n[0], n[1], n[2], dp(n[0]), n[0], n[0])); written_p.add(n[0])
        elif name_ == "fmadds":   # op_fmadds(t, a, b, cc): fma(f[a], f[cc], f[b])
            L.append("    %sf%d = rs(std::fma(f%d, f%d, f%d)); %sp%d = f%d;" % (df(n[0]), n[0], n[1], n[3], n[2], dp(n[0]), n[0], n[0])); written_p.add(n[0])
        elif name_ == "fmsubs":
            L.append("    %sf%d = rs(std::fma(f%d, f%d, -f%d)); %sp%d = f%d;" % (df(n[0]), n[0], n[1], n[3], n[2], dp(n[0]), n[0], n[0])); written_p.add(n[0])
        elif name_ == "fneg":
            L.append("    %sf%d = -f%d;" % (df(n[0]), n[0], n[1]))
        elif name_ == "fmr":
            L.append("    %sf%d = f%d;" % (df(n[0]), n[0], n[1]))
        elif name_ == "fabs":
            L.append("    %sf%d = std::fabs(f%d);" % (df(n[0]), n[0], n[1]))
    # write back the volatile registers the function modified (callee-saved ones are restored by the epilogue)
    for r in sorted(int_written):
        if r == 0 or 3 <= r <= 12:
            L.append("    c.r[%d] = r%d;" % (r, r))
    for r in sorted(f_written):
        if r <= 13:
            L.append("    c.f[%d] = f%d;%s" % (r, r, (" c.ps1[%d] = p%d;" % (r, r)) if r in written_p else ""))
    L.append("}")
    return "\n".join(L)


def main():
    gen_dir = Path(sys.argv[1])
    chosen = []
    for address in sys.argv[2:]:
        body = load_function(gen_dir, address.upper())
        parsed, why = parse(body)
        if not parsed:
            print("rejected %s: %s" % (address, why), file=sys.stderr)
            continue
        chosen.append((address.upper(), parsed))
    root = Path(__file__).resolve().parent.parent
    code = ['// Generated by tools/leaf_hle.py from the translated game: native versions of small straight-line leaf functions. Do not edit.',
            '#include "ppc_rt.h"', '#include "gx_write_watch.h"', '#include <cmath>', '#include <cstring>', '',
            'namespace {', 'using ppc::Ctx;',
            'inline bool in_ram(uint32_t ea, uint32_t size) { return (ea >= 0x80000000u && ea + size <= 0x81800000u) || (ea >= 0x90000000u && ea + size <= 0x94000000u); }',
            'inline bool float_gqr0(const Ctx& c) { return ((c.gqr[0] >> 16) & 7) < 4 && (c.gqr[0] & 7) < 4; }',
            'inline double rs(double v) { return ppc::rnd_s(v); }',
            'inline uint32_t ld32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return _byteswap_ulong(v); }',
            'inline void st32(uint8_t* p, uint32_t v) { v = _byteswap_ulong(v); std::memcpy(p, &v, 4); }',
            'inline double ldf(const uint8_t* p) { const uint32_t b = ld32(p); float f; std::memcpy(&f, &b, 4); return f; }',
            'inline void stf(uint8_t* p, double v) { const float f = float(v); uint32_t b; std::memcpy(&b, &f, 4); st32(p, b); }',
            'inline double ldd(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); v = _byteswap_uint64(v); double d; std::memcpy(&d, &v, 8); return d; }',
            'inline void std_(uint8_t* p, double d) { uint64_t v; std::memcpy(&v, &d, 8); v = _byteswap_uint64(v); std::memcpy(p, &v, 8); }',
            '}  // namespace', '']
    test = ['// Generated by tools/leaf_hle.py: the translated instructions of each leaf function against its native version. Do not edit.',
            '#include "check.h"', '#include "ppc_fixture.h"', '#include <cstdio>', '#include <cstring>', '#include <random>', '#include <string>', '#include <vector>',
            'using namespace ppc;', '']
    mapping = []
    for address, (ops, raw) in chosen:
        name = "Leaf_%s" % address
        code.append(emit_native(address, name, ops, raw))
        code.append("")
        test.append("void hle_%s(Ctx&);" % name)
        mapping.append("@%s %s" % (address, name))
    test += ['', 'namespace {']
    for address, (ops, raw) in chosen:
        test.append("void ref_Leaf_%s(Ctx& c) {" % address)
        for r in raw:
            test.append("    " + r.replace("ppc::", ""))
        test.append("}")
    test += ['constexpr uint32_t kBase = 0x80010000, kSda = 0x80020000, kStack = 0x80013000, kRegion = 0x1000;',
             'std::mt19937_64 rng(0x1EAF);',
             'uint32_t random_float_bits() {',
             '    switch (rng() % 12) {',
             '        case 0: return 0; case 1: return 0x80000000u; case 2: return 0x7F800000u | (uint32_t(rng() & 1) << 31); case 3: return 0x7FC00000u | uint32_t(rng() & 0x3FFFFF);',
             '        case 4: return uint32_t(rng() & 0x007FFFFF) | (uint32_t(rng() & 1) << 31); case 5: return 0x7F000000u + uint32_t(rng() & 0x7FFFFF);',
             '        default: { std::uniform_real_distribution<float> d(-100.f, 100.f); const float v = d(rng); uint32_t b; std::memcpy(&b, &v, 4); return b; }',
             '    }',
             '}',
             'double random_double() { std::uniform_real_distribution<double> d(-50.0, 50.0); switch (rng() % 8) { case 0: return 0.0; case 1: return -0.0; case 2: return 1e30; case 3: return 1e-45; default: return d(rng) * (1.0 + 1e-9); } }',
             'bool is_nan32(uint32_t w) { return (w & 0x7F800000u) == 0x7F800000u && (w & 0x007FFFFFu); }',
             'bool same_double(double a, double b) { uint64_t x, y; std::memcpy(&x, &a, 8); std::memcpy(&y, &b, 8); return x == y || (a != a && b != b); }',
             'struct State { std::vector<uint8_t> memory; uint32_t r[32], lr; double f[32], p[32]; };',
             'State run(void (*fn)(Ctx&), const std::vector<uint32_t>& words, const uint32_t (&ptr)[6], const uint32_t (&ints)[32], const double (&fl)[64], uint32_t lr, const std::vector<std::pair<uint32_t, uint32_t>>& sda) {',
             '    Fix fix; Ctx& c = fix.c;',
             '    for (size_t i = 0; i < words.size(); ++i) wr32(c, kBase + uint32_t(i) * 4, words[i]);',
             '    for (const auto& [at, v] : sda) wr32(c, at, v);',
             '    for (uint32_t i = 0; i < 96; ++i) wr32(c, kStack + i * 4, words[(i * 7) % words.size()]);   // the caller frame: stack arguments',
             '    for (int i = 0; i < 32; ++i) { c.r[i] = ints[i]; c.f[i] = fl[i]; c.ps1[i] = fl[32 + i]; }',
             '    c.r[1] = kStack; c.r[2] = kSda; for (int i = 0; i < 6; ++i) c.r[3 + i] = ptr[i]; c.lr = lr; c.gqr[0] = 0; c.gqr[1] = 0;',
             '    fn(c);',
             '    State s; s.memory.assign(c.mem + (kBase & 0x3FFFFFFF), c.mem + (kBase & 0x3FFFFFFF) + kRegion); s.lr = c.lr;',
             '    for (int i = 0; i < 32; ++i) { s.r[i] = c.r[i]; s.f[i] = c.f[i]; s.p[i] = c.ps1[i]; }',
             '    return s;',
             '}',
             'bool same_state(const State& a, const State& b, std::string& why) {',
             '    for (size_t i = 0; i + 4 <= a.memory.size(); i += 4) { uint32_t x, y; std::memcpy(&x, &a.memory[i], 4); std::memcpy(&y, &b.memory[i], 4); if (x != y && !(is_nan32(_byteswap_ulong(x)) && is_nan32(_byteswap_ulong(y)))) { why = "memory at +" + std::to_string(i); return false; } }',
             '    for (int i = 0; i < 32; ++i) { if (a.r[i] != b.r[i]) { why = "r" + std::to_string(i); return false; } if (!same_double(a.f[i], b.f[i])) { why = "f" + std::to_string(i); return false; } if (!same_double(a.p[i], b.p[i])) { why = "ps1 of f" + std::to_string(i); return false; } }',
             '    if (a.lr != b.lr) { why = "lr"; return false; }',
             '    return true;',
             '}',
             'void compare(const char* name, void (*hle)(Ctx&), void (*ref)(Ctx&), const std::vector<int>& sda_offsets, int iterations = 2000) {',
             '    int mismatches = 0;',
             '    for (int n = 0; n < iterations; ++n) {',
             '        std::vector<uint32_t> words(kRegion / 4);',
             '        for (auto& w : words) w = random_float_bits();',
             '        uint32_t ptr[6];',
             '        for (auto& p : ptr) p = kBase + 0x80 + uint32_t(rng() % 0x60) * 16;',
             '        if (n % 4 == 1) ptr[1] = ptr[0]; else if (n % 4 == 2) ptr[2] = ptr[0]; else if (n % 4 == 3) ptr[1] = ptr[2];',
             '        uint32_t ints[32]; for (auto& v : ints) v = uint32_t(rng());',
             '        double fl[64]; for (auto& v : fl) v = random_double();',
             '        for (int i = 32; i < 64; ++i) fl[i] = double(float(fl[i]));   // the second halves always hold single-precision values (psq_l after psq_st would round them)',
             '        std::vector<std::pair<uint32_t, uint32_t>> sda; for (int off : sda_offsets) sda.push_back({kSda + uint32_t(off), random_float_bits()});',
             '        const uint32_t lr = uint32_t(rng());',
             '        const State want = run(ref, words, ptr, ints, fl, lr, sda), have = run(hle, words, ptr, ints, fl, lr, sda);',
             '        std::string why;',
             '        if (!same_state(want, have, why) && ++mismatches <= 3) std::printf("%s differs at iteration %d: %s\\n", name, n, why.c_str());',
             '    }',
             '    std::printf("%s: %d mismatches in %d randomised runs\\n", name, mismatches, iterations);',
             '    CHECK(mismatches == 0);',
             '}',
             '}  // namespace', '', 'int main() {']
    for address, (ops, raw) in chosen:
        _, _, _, _, _, spans, _ = analyse(ops)
        sda = []
        for b in (2, 13):
            if b in spans:
                for off in range(spans[b][0], spans[b][1], 4):
                    sda.append(off)
        test.append('    compare("Leaf_%s", hle_Leaf_%s, ref_Leaf_%s, {%s});' % (address, address, address, ", ".join(str(x) for x in sda)))
    test += ['    return 0;', '}', '']
    (root / "runtime/hle/leaf_gen.cpp").write_text("\n".join(code), encoding="utf-8")
    (root / "tests/test_leaf_hle.cpp").write_text("\n".join(test), encoding="utf-8")
    print("\n".join(mapping))


if __name__ == "__main__":
    main()
