"""Offline audit of a captured GX resource; never executes its commands."""
import argparse
import json
import re
from pathlib import Path


def inspect(data: bytes) -> dict:
    commands = []
    offset = 0
    while offset < len(data):
        start = offset
        op = data[offset]
        offset += 1

        def take(count: int) -> bytes:
            nonlocal offset
            if count > len(data) - offset:
                raise ValueError(f"truncated command at {start:#x}: needs {count} bytes, has {len(data)-offset}")
            value = data[offset:offset+count]
            offset += count
            return value

        command = {"offset": start}
        if op == 0:
            continue
        if op == 0x08:
            reg = take(1)[0]
            command.update(kind="CP", register=reg, value=int.from_bytes(take(4), "big"))
        elif op == 0x61:
            word = int.from_bytes(take(4), "big")
            command.update(kind="BP", register=word >> 24, value=word & 0xFFFFFF)
        elif op == 0x10:
            header = int.from_bytes(take(4), "big")
            count = (header >> 16) + 1
            payload = take(count * 4)
            command.update(kind="XF", register=header & 0xFFFF,
                           words=[int.from_bytes(payload[i:i+4], "big") for i in range(0,len(payload),4)])
        elif op in (0x20, 0x28, 0x30, 0x38):
            command.update(kind="indexed_XF", opcode=op, value=int.from_bytes(take(4), "big"))
        elif op == 0x40:
            command.update(kind="list_call", address=int.from_bytes(take(4), "big"), length=int.from_bytes(take(4), "big"))
        elif op == 0x48:
            command.update(kind="invalidate_vertex_cache")
        elif (op & 0xF8) in (0x80,0x90,0x98,0xA0,0xA8,0xB0,0xB8):
            count = int.from_bytes(take(2), "big")
            command.update(kind="primitive", primitive=op & 0xF8, vat=op & 7, vertices=count)
            commands.append(command)
            if count:
                return {"complete": False, "stop": "vertex_layout_required", "payload_offset": offset, "commands": commands}
            continue
        else:
            raise ValueError(f"unsupported command {op:#x} at {start:#x}")
        commands.append(command)
    return {"complete": True, "commands": commands}


def resources_from_log(text: str) -> list[tuple[int, bytes]]:
    matches = list(re.finditer(r"^gx resource: address ([0-9A-Fa-f]{8}) size (\d+) captured (\d+) hex((?: [0-9A-Fa-f]{2})*)$",text,re.MULTILINE))
    if not matches:
        raise ValueError("no gx resource capture in log")
    resources = []
    for match in matches:
        size, captured = int(match[2]), int(match[3])
        data = bytes.fromhex(match[4])
        if size != captured or len(data) != captured:
            raise ValueError("resource capture is incomplete")
        resources.append((int(match[1],16),data))
    return resources


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    try:
        report = [{"address":address,"size":len(data),**inspect(data)} for address,data in resources_from_log(args.log.read_text(encoding="utf-8"))]
    except (OSError, UnicodeError, ValueError) as error:
        parser.exit(2, f"GX resource audit failed: {error}\n")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
