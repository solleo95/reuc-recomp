"""Prints the strings of a game text file (Data/message/mes_<lang>.txtres) as JSON: { "<record id>": "line one\nline two" }.

    python dump_text.py <mes_xx.txtres> [--output <file.json>]

The output is the format of a mod's text/<lang>.json (docs/modding/text.md): delete the entries you do not change and edit the others. Text is shown as
the file holds it (UTF-8); some localizations draw their own alphabet through the font's code points, so a string can look odd here.
"""
import argparse
import json
import struct
import sys

MAX_RECORDS = 100000
MAX_LINES = 1000


def read_texts(data):
    """[(id, [lines])] of a TRES file; raises ValueError for a damaged one."""
    if len(data) < 0x10 or data[:4] != b"TRES":
        raise ValueError("not a text file (no TRES header)")
    version, count, table = struct.unpack_from(">III", data, 4)
    if version != 0x00010000:
        raise ValueError("unknown text file version")
    if not 0 < count <= MAX_RECORDS or table < 0x10 or table + 4 * count > len(data):
        raise ValueError("the record table does not fit in the file")
    records, previous = [], -1
    for i in range(count):
        (at,) = struct.unpack_from(">I", data, table + 4 * i)
        if at + 20 > len(data):
            raise ValueError(f"record {i} is outside the file")
        rid, _voice, lines, text, _ = struct.unpack_from(">5I", data, at)
        if rid <= previous:
            raise ValueError(f"the record ids are not ascending at record {i}")
        previous = rid
        if lines > MAX_LINES:
            raise ValueError(f"record {i} claims too many lines")
        found, cursor = [], text
        for _ in range(lines):
            end = data.find(b"\0", cursor) if cursor < len(data) else -1
            if end < 0:
                raise ValueError(f"a line of record {i} is outside the file or has no end")
            found.append(data[cursor:end].decode("utf-8", errors="replace"))
            cursor = end + 1
        records.append((rid, found))
    return records


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("file")
    parser.add_argument("--output", help="write the JSON here (UTF-8) instead of to the console")
    args = parser.parse_args(argv)
    try:
        with open(args.file, "rb") as f:
            records = read_texts(f.read())
    except (OSError, ValueError) as error:
        print(f"dump_text: {args.file}: {error}", file=sys.stderr)
        return 1
    text = json.dumps({str(rid): "\n".join(lines) for rid, lines in records}, ensure_ascii=False, indent=1) + "\n"
    if args.output:
        with open(args.output, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    else:
        sys.stdout.buffer.write(text.encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
