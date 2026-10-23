"""Spike M3: read a U8 archive and rebuild it with changed members, to learn whether the game copes with an archive of another size.

    python tools/spikes/u8_repack.py roundtrip <disc-root>          rebuild every .arc unchanged and compare bytes
    python tools/spikes/u8_repack.py grow <in.arc> <out.arc> <member-path> <extra-bytes>   append zero bytes to one member (later members move)
"""
import os
import struct
import sys

MAGIC = b"\x55\xAA\x38\x2D"
ALIGN = 0x20


def align(value, to=ALIGN):
    return (value + to - 1) & ~(to - 1)


class Node:
    def __init__(self, is_dir, name, a, b):
        self.is_dir, self.name, self.a, self.b = is_dir, name, a, b   # file: a = data offset, b = size; dir: a = parent, b = end index
        self.data = None


def parse(blob):
    if blob[:4] != MAGIC:
        raise ValueError("not a U8 archive")
    root_off, hdr_size, data_off = struct.unpack(">III", blob[4:16])
    count = struct.unpack(">I", blob[root_off + 8:root_off + 12])[0]
    names_at = root_off + count * 12
    nodes = []
    for i in range(count):
        o = root_off + i * 12
        flag = blob[o]
        name_off = int.from_bytes(blob[o + 1:o + 4], "big")
        a, b = struct.unpack(">II", blob[o + 4:o + 12])
        end = blob.index(b"\0", names_at + name_off)
        node = Node(flag == 1, blob[names_at + name_off:end].decode("latin-1"), a, b)
        if not node.is_dir:
            node.data = blob[a:a + b]
        nodes.append(node)
    return dict(nodes=nodes, root_off=root_off, hdr_size=hdr_size, data_off=data_off, names_at=names_at, header_blob=blob[:data_off])


def paths(archive):
    """Full path of every node, in node order."""
    result, stack = [], []   # stack of (end_index, path)
    for i, node in enumerate(archive["nodes"]):
        while stack and i >= stack[-1][0]:
            stack.pop()
        base = stack[-1][1] if stack else ""
        full = base if i == 0 else (base + "/" + node.name if base else node.name)
        result.append(full)
        if node.is_dir:
            stack.append((node.b, full))
    return result


def build(archive, replace=None, head_pad=0):
    """Same tree, same node and string tables; members packed in their original order with 0x20 alignment. `replace` maps a member path to new bytes;
    `head_pad` moves the data area that many bytes further from the tables (a multiple of 0x20)."""
    replace = replace or {}
    nodes, names = archive["nodes"], paths(archive)
    order = sorted((i for i, n in enumerate(nodes) if not n.is_dir), key=lambda i: nodes[i].a)
    out_offsets, cursor, body = {}, archive["data_off"] + head_pad, bytearray()
    for i in order:
        data = replace.get(names[i], nodes[i].data)
        out_offsets[i] = (cursor, len(data))
        body += data
        pad = align(cursor + len(data)) - (cursor + len(data))
        body += b"\0" * pad
        cursor += len(data) + pad
    head = bytearray(archive["header_blob"]) + b"\0" * head_pad
    head[12:16] = struct.pack(">I", archive["data_off"] + head_pad)
    for i, (offset, size) in out_offsets.items():
        o = archive["root_off"] + i * 12
        head[o + 4:o + 12] = struct.pack(">II", offset, size)
    return bytes(head) + bytes(body)


def trimmed(original, rebuilt):
    """The original may lack padding after its last member; ignore trailing zeros when comparing."""
    return original.rstrip(b"\0") == rebuilt.rstrip(b"\0")


def main(argv):
    if argv[1] == "roundtrip":
        total = same = 0
        for dirpath, _, files in os.walk(argv[2]):
            for name in files:
                if not name.lower().endswith(".arc"):
                    continue
                path = os.path.join(dirpath, name)
                blob = open(path, "rb").read()
                total += 1
                if trimmed(blob, build(parse(blob))):
                    same += 1
                else:
                    print("differs:", path)
        print(f"{same}/{total} archives rebuilt identically")
    elif argv[1] == "grow":
        blob = open(argv[2], "rb").read()
        archive = parse(blob)
        names = paths(archive)
        index = names.index(argv[4])
        grown = archive["nodes"][index].data + b"\0" * int(argv[5])
        open(argv[3], "wb").write(build(archive, {argv[4]: grown}))
        print(f"{argv[2]}: {len(blob)} -> {os.path.getsize(argv[3])} bytes")
    else:
        print(__doc__)


if __name__ == "__main__":
    main(sys.argv)
