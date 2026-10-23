"""Lists, unpacks and packs the game's U8 archives (.arc), for mod authors (docs/modding/archives.md).

    python arc_tool.py list   <file.arc>
    python arc_tool.py unpack <file.arc> <folder>      the folder must not exist or must be empty
    python arc_tool.py pack   <folder> <file.arc>

Only needed to look inside an archive or to ship a whole archive: a mod can replace a single member by putting a file at
files/<disc path>.arc!/<path inside> and leaving the packing to the game's port.
"""
import os
import struct
import sys

MAGIC = b"\x55\xAA\x38\x2D"
MAX_NODES = 100000
MAX_DEPTH = 64


class ArcError(Exception):
    pass


def align(value):
    return (value + 0x1F) & ~0x1F


def parse(data):
    """[(path, bytes)] of the files of a U8 archive in node order; raises ArcError for a damaged one."""
    if len(data) < 0x20 or data[:4] != MAGIC:
        raise ArcError("not a U8 archive")
    root_off, tables, data_off = struct.unpack_from(">III", data, 4)
    if root_off < 0x20 or root_off + tables > len(data) or tables < 12 or data[root_off] != 1:
        raise ArcError("the node table lies outside the file")
    if data_off < root_off + tables or data_off > len(data):
        raise ArcError("the data area lies inside the tables or outside the file")
    (count,) = struct.unpack_from(">I", data, root_off + 8)
    if not 0 < count <= MAX_NODES or count * 12 > tables:
        raise ArcError("bad node count")
    pool = root_off + count * 12
    pool_end = root_off + tables
    files, open_dirs = [], [(count, "")]   # (end index, path)
    for i in range(1, count):
        flag, hi, mid, lo, a, b = struct.unpack_from(">BBBBII", data, root_off + i * 12)
        while len(open_dirs) > 1 and i >= open_dirs[-1][0]:
            open_dirs.pop()
        name_at = pool + ((hi << 16) | (mid << 8) | lo)
        end = data.find(b"\0", name_at, pool_end)
        if flag > 1 or name_at >= pool_end or end < 0:
            raise ArcError(f"node {i} has a bad type or name")
        name = data[name_at:end].decode("utf-8", "replace")
        if not name or name in (".", "..") or any(c in name for c in "/\\:\0"):
            raise ArcError(f"node {i} has an unusable name {name!r}")
        path = (open_dirs[-1][1] + "/" if open_dirs[-1][1] else "") + name
        if flag == 1:
            if b <= i or b > open_dirs[-1][0] or len(open_dirs) >= MAX_DEPTH:
                raise ArcError(f"folder {path!r} has a bad range")
            open_dirs.append((b, path))
        else:
            if a < data_off or a + b > len(data):
                raise ArcError(f"member {path!r} lies outside the file")
            files.append((path, data[a:a + b]))
    return files


def build(files):
    """U8 archive bytes for [(path, bytes)]: folders and files sorted by name (ASCII case-insensitive), depth first, members 0x20 aligned."""
    tree = {}
    for path, content in files:
        node = tree
        parts = path.split("/")
        for part in parts[:-1]:
            node = node.setdefault(part, {})
            if not isinstance(node, dict):
                raise ArcError(f"{path!r}: {part!r} is both a file and a folder")
        if isinstance(node.get(parts[-1]), dict):
            raise ArcError(f"{path!r} is both a file and a folder")
        node[parts[-1]] = content
    nodes, pool = [], bytearray(b"\0")   # (is_dir, name_offset, parent_or_offset, end_or_size, content)

    def visit(name, value, parent):
        index = len(nodes)
        name_off = len(pool)
        pool.extend(name.encode("utf-8") + b"\0")
        if isinstance(value, dict):
            nodes.append([True, name_off, parent, 0, None])
            for child in sorted(value, key=lambda n: (n.lower(), n)):
                visit(child, value[child], index)
            nodes[index][3] = len(nodes)
        else:
            nodes.append([False, name_off, 0, len(value), value])

    nodes.append([True, 0, 0, 0, None])
    for child in sorted(tree, key=lambda n: (n.lower(), n)):
        visit(child, tree[child], 0)
    nodes[0][3] = len(nodes)
    tables = len(nodes) * 12 + len(pool)
    data_off = align(0x20 + tables)
    cursor = data_off
    for node in nodes:
        if not node[0]:
            node[2] = cursor
            cursor += align(len(node[4]))
    out = bytearray(cursor)
    out[0:4] = MAGIC
    struct.pack_into(">III", out, 4, 0x20, tables, data_off)
    for i, (is_dir, name_off, a, b, content) in enumerate(nodes):
        struct.pack_into(">BBBBII", out, 0x20 + i * 12, 1 if is_dir else 0, name_off >> 16, (name_off >> 8) & 255, name_off & 255, a, b)
        if not is_dir:
            out[a:a + len(content)] = content
    out[0x20 + len(nodes) * 12:0x20 + tables] = pool
    return bytes(out)


def main(argv):
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8")   # names in archives are UTF-8; the console's code page must not break a listing
    if len(argv) < 3 or argv[1] not in ("list", "unpack", "pack"):
        print(__doc__, file=sys.stderr)
        return 2
    try:
        if argv[1] == "list":
            for path, content in parse(open(argv[2], "rb").read()):
                print(f"{len(content)}\t{path}")
        elif argv[1] == "unpack":
            if len(argv) != 4:
                raise ArcError("unpack needs an archive and a folder")
            folder = argv[3]
            if os.path.isdir(folder) and os.listdir(folder):
                raise ArcError(f"{folder} is not empty")
            files = parse(open(argv[2], "rb").read())
            for path, content in files:
                target = os.path.join(folder, *path.split("/"))
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with open(target, "wb") as out:
                    out.write(content)
            print(f"{len(files)} files written to {folder}")
        else:
            if len(argv) != 4:
                raise ArcError("pack needs a folder and an archive")
            files = []
            for dirpath, _, names in os.walk(argv[2]):
                for name in names:
                    full = os.path.join(dirpath, name)
                    files.append((os.path.relpath(full, argv[2]).replace(os.sep, "/"), open(full, "rb").read()))
            if not files:
                raise ArcError(f"{argv[2]} holds no files")
            with open(argv[3], "wb") as out:
                out.write(build(files))
            print(f"{len(files)} files packed into {argv[3]}")
    except (ArcError, OSError) as error:
        print(f"arc_tool: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
