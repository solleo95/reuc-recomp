"""Spike for docs/superpowers/specs/2026-10-22-asset-mods-design.md: rewrites a DSP-ADPCM BRSTM as a PCM16 BRSTM (same samples, same loop), to see
whether the game's stream player accepts codec 1.

    python tools/spikes/brstm_pcm16.py <in.brstm> <out.brstm> [--no-adpc] [--silence]

The HEAD chunk (track and channel tables) is kept; only the stream info changes (codec, block layout). The ADPC chunk (seek table of ADPCM history) is
zero-filled by default, or left out with --no-adpc.
"""
import struct
import sys

PCM_BLOCK_BYTES = 0x2000   # per channel; 4096 samples


def u32(d, o): return struct.unpack(">I", d[o:o + 4])[0]


def align(n, a): return (n + a - 1) // a * a


def parse(d):
    assert d[:4] == b"RSTM", "not a BRSTM"
    head_off, head_size, adpc_off, adpc_size, data_off, data_size = struct.unpack(">6I", d[0x10:0x28])
    base = head_off + 8
    stream_off, track_off, chan_off = u32(d, base + 4), u32(d, base + 12), u32(d, base + 20)
    si = base + stream_off
    info = dict(codec=d[si], loop=d[si + 1], channels=d[si + 2], rate=struct.unpack(">H", d[si + 4:si + 6])[0],
                loop_start=u32(d, si + 8), total=u32(d, si + 12), data_start=u32(d, si + 16), blocks=u32(d, si + 20), block_size=u32(d, si + 24),
                block_samples=u32(d, si + 28), last_size=u32(d, si + 32), last_samples=u32(d, si + 36), last_padded=u32(d, si + 40))
    ctab = base + chan_off
    count = d[ctab]   # a byte, then 3 bytes of padding
    coefs = []
    for c in range(count):
        entry = u32(d, ctab + 4 + c * 8 + 4)
        param = base + u32(d, base + entry + 4)   # channel info -> ADPCM parameters
        coefs.append(struct.unpack(">16h", d[param:param + 32]))
    return dict(head=(head_off, head_size), adpc=(adpc_off, adpc_size), data=(data_off, data_size), si=si, **info), coefs


def decode_adpcm(d, info, coefs):
    channels, out = info["channels"], [[] for _ in range(info["channels"])]
    hist = [[0, 0] for _ in range(channels)]
    for block in range(info["blocks"]):
        last = block == info["blocks"] - 1
        size, samples = (info["last_size"], info["last_samples"]) if last else (info["block_size"], info["block_samples"])
        stride = info["last_padded"] if last else info["block_size"]
        for c in range(channels):
            start = info["data_start"] + block * info["block_size"] * channels + c * stride
            data = d[start:start + stride]
            y1, y2 = hist[c]
            produced = 0
            for frame in range(0, len(data), 8):
                if produced >= samples: break
                ps = data[frame]
                scale, predictor = 1 << (ps & 15), ps >> 4
                c1, c2 = coefs[c][predictor * 2], coefs[c][predictor * 2 + 1]
                for i in range(14):
                    if produced >= samples: break
                    byte = data[frame + 1 + i // 2]
                    nib = (byte >> 4) if i % 2 == 0 else (byte & 15)
                    nib -= 16 if nib >= 8 else 0
                    v = (nib * scale * 2048 + 1024 + c1 * y1 + c2 * y2) >> 11
                    v = max(-32768, min(32767, v))
                    out[c].append(v)
                    y2, y1 = y1, v
                    produced += 1
            hist[c] = [y1, y2]
    return out


def write_pcm16(src, info, pcm, with_adpc):
    channels, total = info["channels"], len(pcm[0])
    spb = PCM_BLOCK_BYTES // 2
    blocks = (total + spb - 1) // spb
    last_samples = total - (blocks - 1) * spb
    last_size = last_samples * 2
    last_padded = align(last_size, 0x20)
    head_off, head_size = info["head"]
    head = bytearray(src[head_off:head_off + head_size])
    si = info["si"] - head_off
    head[si] = 1   # codec: PCM16
    for off, value in ((0x10, 0), (0x14, blocks), (0x18, PCM_BLOCK_BYTES), (0x1C, spb), (0x20, last_size), (0x24, last_samples), (0x28, last_padded)):
        if off == 0x10: continue   # data start is patched below
        head[si + off:si + off + 4] = struct.pack(">I", value)
    adpc_size = align(8 + blocks * channels * 4, 0x20) if with_adpc else 0
    head_pos = 0x40
    adpc_pos = head_pos + head_size
    data_pos = adpc_pos + adpc_size
    head[si + 0x10:si + 0x14] = struct.pack(">I", data_pos + 0x20)
    body = bytearray()
    for block in range(blocks):
        last = block == blocks - 1
        for c in range(channels):
            chunk = pcm[c][block * spb:(block + 1) * spb]
            raw = struct.pack(">%dh" % len(chunk), *chunk)
            body += raw + b"\0" * ((last_padded if last else PCM_BLOCK_BYTES) - len(raw))
    data_chunk = b"DATA" + struct.pack(">I", 0x20 + len(body)) + b"\0" * 0x18 + body
    data_chunk += b"\0" * (align(len(data_chunk), 0x20) - len(data_chunk))
    adpc = (b"ADPC" + struct.pack(">I", adpc_size) + b"\0" * (adpc_size - 8)) if with_adpc else b""
    chunks = 3 if with_adpc else 2
    header = b"RSTM" + b"\xFE\xFF" + b"\x01\x00" + struct.pack(">I", data_pos + len(data_chunk)) + struct.pack(">HH", 0x40, chunks)
    header += struct.pack(">6I", head_pos, head_size, adpc_pos if with_adpc else 0, adpc_size, data_pos, len(data_chunk))
    header = header.ljust(0x40, b"\0")
    # the loop start is a sample index and stays valid; total samples is unchanged
    return header + bytes(head) + adpc + data_chunk


def main(argv):
    src = open(argv[1], "rb").read()
    info, coefs = parse(src)
    assert info["codec"] == 2, "input must be DSP-ADPCM"
    pcm = decode_adpcm(src, info, coefs)
    assert len(pcm[0]) == info["total"], (len(pcm[0]), info["total"])
    if "--silence" in argv:   # control for the in-game check: same stream, all zeros
        pcm = [[0] * len(channel) for channel in pcm]
    out = write_pcm16(src, info, pcm, "--no-adpc" not in argv)
    open(argv[2], "wb").write(out)
    print("%s: %d channel(s), %d Hz, %d samples, loop=%d@%d -> %d bytes PCM16 (was %d)" % (
        argv[1], info["channels"], info["rate"], info["total"], info["loop"], info["loop_start"], len(out), len(src)))


if __name__ == "__main__":
    main(sys.argv)
