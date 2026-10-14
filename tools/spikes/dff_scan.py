"""Scan a Dolphin FIFO log (.dff): per-primitive XF lighting state and first-vertex view position/normal.

Diagnostic reference tool (reads a capture made in the user's own emulator; no Dolphin code).
Usage: python -I dff_scan.py file.dff [frame] > out.txt
"""
import struct, sys

last_op = 0
initial_bp = []
FMT_SIZE = [1, 1, 2, 2, 4]  # u8 s8 u16 s16 f32
COL_SIZE = [2, 3, 4, 2, 3, 4]


def be32(b, o): return struct.unpack_from(">I", b, o)[0]


class Ram:
    def __init__(self): self.pages = {}
    def write(self, addr, data):
        addr &= 0x1FFFFFFF
        for i in range(0, len(data), 1 << 16):
            for j, byte in enumerate(data[i:i + (1 << 16)]):
                pass
        self.pages[addr] = bytes(data)
    def read(self, addr, size):
        addr &= 0x1FFFFFFF
        for base, data in self.pages.items():
            if base <= addr and addr + size <= base + len(data):
                return data[addr - base: addr - base + size]
        return None


def load(path):
    d = open(path, "rb").read()
    h = struct.unpack_from("<IIIQIQIQIQIQIIQIII8s", d, 0)
    (fid, ver, _, bp_o, bp_n, cp_o, cp_n, xf_o, xf_n, xr_o, xr_n, fl_o, frames, flags, tm_o, tm_n, m1, m2, gid) = h
    assert fid == 0x0D01F1F0, hex(fid)
    cp = list(struct.unpack_from("<%dI" % (cp_n), d, cp_o)) if cp_n else []
    global initial_bp
    initial_bp = list(struct.unpack_from("<%dI" % (bp_n), d, bp_o)) if bp_n else []
    xf = list(struct.unpack_from("<%dI" % (xf_n), d, xf_o))
    xr = list(struct.unpack_from("<%dI" % (xr_n), d, xr_o))
    out = []
    for f in range(frames):
        fo, fn, fs, fe, mo, mn = struct.unpack_from("<QIIIQI", d, fl_o + f * 64)
        updates = []
        for u in range(mn):
            pos, addr, do, dn, typ = struct.unpack_from("<IIQIB", d, mo + u * 24)
            updates.append((pos, addr, d[do:do + dn], typ))
        out.append((d[fo:fo + fn], updates))
    return gid, cp, xf, xr, out


class State:
    def __init__(self, cp, xf, xr):
        self.cp = dict(enumerate(cp)) if cp else {}
        self.xf = {i: v for i, v in enumerate(xf)}  # XF memory 0x0000..0x0FFF
        for i, v in enumerate(xr): self.xf[0x1000 + i] = v  # XF registers 0x1000..
        self.bp = {i: v & 0xFFFFFF for i, v in enumerate(initial_bp)}


def f32(v): return struct.unpack(">f", struct.pack(">I", v & 0xFFFFFFFF))[0]


def vertex_layout(st, vat):
    lo, hi = st.cp.get(0x50, 0), st.cp.get(0x60, 0)
    a, b, c = st.cp.get(0x70 + vat, 0), st.cp.get(0x80 + vat, 0), st.cp.get(0x90 + vat, 0)
    attrs = []
    if lo & 1: attrs.append(("pnmtx", 1, 1, None))
    for t in range(8):
        if lo >> (1 + t) & 1: attrs.append((f"t{t}mtx", 1, 1, None))
    pos_desc, nrm_desc, c0, c1 = lo >> 9 & 3, lo >> 11 & 3, lo >> 13 & 3, lo >> 15 & 3
    pos_cnt, pos_fmt, pos_frac = 3 if a & 1 else 2, a >> 1 & 7, a >> 4 & 31
    nrm_nbt, nrm_fmt, nrm_idx3 = a >> 9 & 1, a >> 10 & 7, a >> 31 & 1
    attrs.append(("pos", pos_desc, pos_cnt * FMT_SIZE[pos_fmt], (pos_cnt, pos_fmt, pos_frac, 0)))
    if nrm_desc:
        cnt = 9 if nrm_nbt else 3
        size = cnt * FMT_SIZE[nrm_fmt]
        if nrm_desc >= 2 and nrm_nbt and nrm_idx3: attrs.append(("nrm3", nrm_desc, 3 * (1 if nrm_desc == 2 else 2), None))
        else: attrs.append(("nrm", nrm_desc, size, (3, nrm_fmt, 6 if nrm_fmt == 1 else 14 if nrm_fmt == 3 else 0, 10)))
    else: attrs.append(("nrm", 0, 0, None))
    attrs.append(("c0", c0, COL_SIZE[a >> 14 & 7], None))
    attrs.append(("c1", c1, COL_SIZE[a >> 18 & 7], None))
    tex = [(a >> 21 & 1, a >> 22 & 7), (b & 1, b >> 1 & 7), (b >> 9 & 1, b >> 10 & 7), (b >> 18 & 1, b >> 19 & 7),
           (b >> 27 & 1, b >> 28 & 7), (c >> 5 & 1, c >> 6 & 7), (c >> 14 & 1, c >> 15 & 7), (c >> 23 & 1, c >> 24 & 7)]
    for t in range(8):
        attrs.append((f"tex{t}", hi >> (2 * t) & 3, (2 if tex[t][0] else 1) * FMT_SIZE[tex[t][1]], None))
    return attrs


def attr_size(desc, direct):
    return 0 if desc == 0 else direct if desc == 1 else 1 if desc == 2 else 2


def read_values(raw, cnt, fmt, frac):
    size = FMT_SIZE[fmt]
    code = ">" + "BbHhf"[fmt] * cnt
    vals = struct.unpack_from(code, raw, 0)
    return [v / (1 << frac) if fmt != 4 else v for v in vals]


def decode(st, ram, data, off, attrs, array_slot):
    """Return (pnmtx index, position, normal) of the vertex at data[off]."""
    pn, pos, nrm = st.xf.get(0x1018, 0) & 63, None, None
    for name, desc, direct, info in attrs:
        n = attr_size(desc, direct)
        raw = data[off:off + n]
        if name == "pnmtx": pn = raw[0] & 63
        elif name in ("pos", "nrm") and desc and info:
            if desc == 1: src = raw
            else:
                idx = raw[0] if desc == 2 else struct.unpack(">H", raw)[0]
                slot = array_slot[name]
                base, stride = st.cp.get(0xA0 + slot, 0), st.cp.get(0xB0 + slot, 0) & 0xFF
                src = ram.read(base + idx * stride, info[0] * FMT_SIZE[info[1]])
            if src is not None:
                vals = read_values(src, info[0], info[1], info[2])
                if name == "pos": pos = vals + [0.0] * (3 - len(vals))
                else: nrm = vals
        off += n
    return pn, pos, nrm


def walk(st, ram, data, report, depth=0):
    o = 0
    while o < len(data):
        op = data[o]; o += 1
        global last_op; last_op = op
        if op == 0x00 or op == 0x48: continue
        if op == 0x08:
            reg, val = data[o], be32(data, o + 1); o += 5; st.cp[reg] = val
        elif op == 0x10:
            hdr = be32(data, o); o += 4
            n, addr = (hdr >> 16) + 1, hdr & 0xFFFF
            for i in range(n): st.xf[addr + i] = be32(data, o + 4 * i)
            o += 4 * n
        elif op in (0x20, 0x28, 0x30, 0x38):
            v = be32(data, o); o += 4
            idx, n, addr = v >> 16, (v >> 12 & 15) + 1, v & 0xFFF
            slot = 12 + (op - 0x20) // 8
            base, stride = st.cp.get(0xA0 + slot, 0), st.cp.get(0xB0 + slot, 0) & 0xFFF
            raw = ram.read(base + idx * stride, 4 * n)
            if raw:
                for i in range(n): st.xf[addr + i] = be32(raw, 4 * i)
        elif op == 0x40:
            addr, size = be32(data, o), be32(data, o + 4); o += 8
            dl = ram.read(addr, size)
            if dl is not None and depth < 8: walk(st, ram, dl, report, depth + 1)
            else: report(("missing-dl", addr, size))
        elif op == 0x61:
            v = be32(data, o); o += 4
            reg, val = v >> 24, v & 0xFFFFFF
            if reg == 0xFE: st.bp_mask = val
            else:
                mask = getattr(st, "bp_mask", 0xFFFFFF)
                st.bp[reg] = (st.bp.get(reg, 0) & ~mask) | (val & mask); st.bp_mask = 0xFFFFFF
        elif op & 0x80:
            vat, prim = op & 7, op & 0xF8
            count = struct.unpack_from(">H", data, o)[0]; o += 2
            attrs = vertex_layout(st, vat)
            vsize = sum(attr_size(d_, s) for _, d_, s, _ in attrs)
            first = decode(st, ram, data, o, attrs, {"pos": 0, "nrm": 1}) if count else None
            report(("prim", prim, vat, count, depth, dict(st.xf), dict(st.cp), first, attrs, dict(st.bp)))
            o += vsize * count
        else:
            report(("bad-op", op, o - 1)); return


def lights_for(xf, ctrl):
    mask = (ctrl >> 2 & 15) | (ctrl >> 7 & 0xF0)
    out = []
    for l in range(8):
        if mask >> l & 1:
            b = 0x600 + l * 16
            col = xf.get(b + 3, 0)
            vals = [f32(xf.get(b + 4 + i, 0)) for i in range(12)]
            out.append((l, col, vals))
    return out


def main():
    gid, cp, xf, xr, frames = load(sys.argv[1])
    which = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    st = State(cp, xf, xr)
    ram = Ram()
    data, updates = frames[which]
    for _, addr, payload, _ in updates: ram.write(addr, payload)
    n = [0]
    def report(item):
        if item[0] != "prim":
            print(item[:3]); return
        _, prim, vat, count, depth, x, c, first, attrs, bp = item
        n[0] += 1
        ctrl = [x.get(0x100E + i, 0) for i in range(4)]
        amb, mat = x.get(0x100A, 0), x.get(0x100C, 0)
        line = f"prim {n[0]} op {prim:02X} vat {vat} count {count} dl {depth} chans {x.get(0x1009,0)} ctrl {ctrl[0]:08X}:{ctrl[1]:08X}:{ctrl[2]:08X}:{ctrl[3]:08X} amb {amb:08X} mat {mat:08X}"
        if first and first[1] is not None:
            pn, pos, nrm = first
            row = lambda r: [f32(x.get(pn * 4 + r * 4 + k, 0)) for k in range(4)]
            view = [sum(row(r)[k] * pos[k] for k in range(3)) + row(r)[3] for r in range(3)]
            line += f" view {view[0]:.2f},{view[1]:.2f},{view[2]:.2f}"
            if nrm:
                nb = 0x400 + (pn % 32) * 3
                nr = lambda r: [f32(x.get(nb + r * 3 + k, 0)) for k in range(3)]
                vn = [sum(nr(r)[k] * nrm[k] for k in range(3)) for r in range(3)]
                line += f" normal {vn[0]:.3f},{vn[1]:.3f},{vn[2]:.3f} raw {nrm[0]:.3f},{nrm[1]:.3f},{nrm[2]:.3f}"
        print(line)
        if ctrl[0] & 2:
            for l, col, v in lights_for(x, ctrl[0]):
                print(f"   light {l} color {col:08X} cos {v[0]:g},{v[1]:g},{v[2]:g} dist {v[3]:g},{v[4]:g},{v[5]:g} pos {v[6]:g},{v[7]:g},{v[8]:g} dir {v[9]:g},{v[10]:g},{v[11]:g}")
    walk(st, ram, data, report)


if __name__ == "__main__":
    main()
