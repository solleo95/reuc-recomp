#!/usr/bin/env python3
"""Generates the mod API documentation and typings from modding/api/uc_api.json.

    python scripts/gen_mod_api.py           write the generated files
    python scripts/gen_mod_api.py --check   exit 1 when a generated file differs from the schema

Outputs: docs/modding/api/*.md, docs/modding/uc.d.ts, docs/modding/lua/uc.lua, modding/generated/hookable.inc.
"""
import argparse
import json
import pathlib
import re
import sys

KINDS = {"function", "property", "event"}
STATUSES = {"implemented", "planned", "unverified"}
ACCESS = {"r", "w", "rw"}
PROVIDERS = {"registry", "context"}
SCOPES = {"slot", "name", "id"}
SCALARS = {"int", "float", "bool", "string", "void", "any", "null", "vec3", "object", "function"}


def load_schema(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def version_tuple(text):
    return tuple(int(p) for p in text.split("."))


def type_errors(type_text, where):
    """Checks a type expression: scalars, array<T>, and unions with '|'."""
    errors = []
    for part in type_text.split("|"):
        part = part.strip()
        inner = re.fullmatch(r"array<(.+)>", part)
        if inner:
            errors += type_errors(inner.group(1), where)
        elif part not in SCALARS:
            errors.append("%s: unknown type '%s'" % (where, part))
    return errors


def validate(schema):
    errors = []
    version = schema.get("version", "")
    if not re.fullmatch(r"\d+\.\d+", version):
        errors.append("version must look like 1.0")
        return errors
    seen_domains = set()
    for domain in schema.get("domains", []):
        dname = domain.get("name", "?")
        if dname in seen_domains:
            errors.append("duplicate domain %s" % dname)
        seen_domains.add(dname)
        if domain.get("provider") not in PROVIDERS:
            errors.append("%s: provider must be one of %s" % (dname, sorted(PROVIDERS)))
        if "scope" in domain and domain["scope"] not in SCOPES:
            errors.append("%s: unknown scope" % dname)
        seen = set()
        for m in domain.get("members", []):
            where = "%s.%s" % (dname, m.get("name", "?"))
            if m.get("name") in seen:
                errors.append("%s: duplicate member" % where)
            seen.add(m.get("name"))
            if m.get("kind") not in KINDS:
                errors.append("%s: kind must be one of %s" % (where, sorted(KINDS)))
            if m.get("status") not in STATUSES:
                errors.append("%s: status must be one of %s" % (where, sorted(STATUSES)))
            if not re.fullmatch(r"\d+\.\d+", m.get("since", "")) or (
                re.fullmatch(r"\d+\.\d+", m.get("since", "")) and version_tuple(m["since"]) > version_tuple(version)
            ):
                errors.append("%s: since must be a version not newer than %s" % (where, version))
            if m.get("kind") == "property" and m.get("access") not in ACCESS:
                errors.append("%s: a property needs access r, w or rw" % where)
            if not m.get("doc"):
                errors.append("%s: doc is required" % where)
            errors += type_errors(m.get("type", ""), where)
            for arg in m.get("args", []):
                errors += type_errors(arg.get("type", ""), "%s(%s)" % (where, arg.get("name", "?")))
                if not arg.get("doc"):
                    errors.append("%s(%s): doc is required" % (where, arg.get("name", "?")))
            if m.get("status") == "implemented" and not (m.get("example", {}).get("lua") and m.get("example", {}).get("js")):
                errors.append("%s: an implemented member needs a lua and a js example" % where)
    seen_hooks = set()
    for hook in schema.get("hookable", []):
        if not re.fullmatch(r"[0-9A-F]{8}", hook.get("address", "")):
            errors.append("hookable %s: address must be 8 upper-case hex digits" % hook.get("name"))
        if hook.get("name") in seen_hooks or hook.get("address") in seen_hooks:
            errors.append("hookable %s: duplicate name or address" % hook.get("name"))
        seen_hooks.add(hook.get("name"))
        seen_hooks.add(hook.get("address"))
        if hook.get("status") not in STATUSES:
            errors.append("hookable %s: bad status" % hook.get("name"))
    return errors


# ---- TypeScript -------------------------------------------------------------------------------------------------------

def ts_type(t):
    out = []
    for part in (p.strip() for p in t.split("|")):
        inner = re.fullmatch(r"array<(.+)>", part)
        if inner:
            out.append(ts_type(inner.group(1)) + "[]" if "|" not in inner.group(1) else "(" + ts_type(inner.group(1)) + ")[]")
        else:
            out.append({"int": "number", "float": "number", "bool": "boolean", "string": "string", "void": "void", "any": "any",
                        "null": "null", "vec3": "Vec3", "object": "Record<string, any>", "function": "(...args: any[]) => any"}[part])
    return " | ".join(out)


def ts_args(m):
    return ", ".join("%s%s: %s" % (a["name"], "?" if a.get("optional") else "", ts_type(a["type"])) for a in m.get("args", []))


def camel(name):
    return name[:1].upper() + name[1:]


def ts_member(m, indent, in_interface=False):
    pad = " " * indent
    doc = "%s/** %s [%s] */\n" % (pad, m["doc"].replace("*/", "* /"), m["status"])
    if m["kind"] == "function":
        return doc + "%s%s%s(%s): %s;\n" % (pad, "" if in_interface else "function ", m["name"], ts_args(m), ts_type(m["type"]))
    ro = "readonly " if m["access"] == "r" else ""
    if in_interface:
        return doc + "%s%s%s: %s;\n" % (pad, ro, m["name"], ts_type(m["type"]))
    return doc + "%s%s %s: %s;\n" % (pad, "const" if m["access"] == "r" else "let", m["name"], ts_type(m["type"]))


def render_dts(schema):
    out = ["// Generated by scripts/gen_mod_api.py from modding/api/uc_api.json (API %s). Do not edit.\n\n" % schema["version"],
           "interface Vec3 { x: number; y: number; z: number; }\n\n"]
    registry = [d for d in schema["domains"] if d["provider"] == "registry"]
    context = [d for d in schema["domains"] if d["provider"] == "context"]
    for d in registry:
        if "scope" in d:
            out.append("/** %s */\ninterface %sHandle {\n" % (d["doc"], camel(d["name"])))
            out += [ts_member(m, 4, True) for m in d["members"] if m["kind"] != "event"]
            out.append("}\n\n")
    out.append("declare namespace uc {\n")
    for d in registry:
        if "scope" in d:
            argtype = {"slot": "slot: 1 | 2", "name": "name: string", "id": "id: number"}[d["scope"]]
            out.append("    /** %s */\n    function %s(%s): %sHandle;\n" % (d["doc"], d["name"], argtype, camel(d["name"])))
        else:
            out.append("    /** %s */\n    namespace %s {\n" % (d["doc"], d["name"]))
            out += [ts_member(m, 8) for m in d["members"] if m["kind"] != "event"]
            out.append("    }\n")
    out.append("}\n\n")
    events = [m["name"] for d in context if d["name"] == "events" for m in d["members"] if m["kind"] == "event"]
    for d in context:
        if d["name"] == "events":
            for e in events:
                out.append('declare function on(event: "%s", handler: (payload: any) => void): void;\n' % e)
            out.append("declare function on(event: string, handler: (payload: any) => void): void;\n\n")
            continue
        out.append("/** %s */\ndeclare namespace %s {\n" % (d["doc"], d["name"]))
        out += [ts_member(m, 4) for m in d["members"] if m["kind"] != "event"]
        out.append("}\n\n")
    return "".join(out).rstrip("\n") + "\n"


# ---- Lua --------------------------------------------------------------------------------------------------------------

def lua_type(t):
    out = []
    for part in (p.strip() for p in t.split("|")):
        inner = re.fullmatch(r"array<(.+)>", part)
        if inner:
            out.append(lua_type(inner.group(1)) + "[]")
        else:
            out.append({"int": "integer", "float": "number", "bool": "boolean", "string": "string", "void": "nil", "any": "any", "null": "nil",
                        "vec3": "{x: number, y: number, z: number}", "object": "table", "function": "function"}[part])
    return "|".join(out)


def render_lua(schema):
    out = ["---@meta\n-- Generated by scripts/gen_mod_api.py from modding/api/uc_api.json (API %s). Do not edit.\n\n" % schema["version"]]
    for d in schema["domains"]:
        if d["provider"] == "registry" and "scope" in d:
            out.append("---@class Uc%sHandle\n" % camel(d["name"]))
            for m in d["members"]:
                if m["kind"] == "property":
                    out.append("---@field %s %s %s [%s]\n" % (m["name"], lua_type(m["type"]), m["doc"].replace("\n", " "), m["status"]))
            out.append("\n")
    out.append("---@class Uc\nuc = {}\n\n")
    for d in schema["domains"]:
        if d["provider"] == "registry":
            if "scope" in d:
                arg = {"slot": "slot integer", "name": "name string", "id": "id integer"}[d["scope"]]
                out.append("---%s\n---@param %s\n---@return Uc%sHandle\nfunction uc.%s(%s) end\n\n" % (d["doc"], arg, camel(d["name"]), d["name"], arg.split()[0]))
                continue
            out.append("---%s\nuc.%s = {}\n\n" % (d["doc"], d["name"]))
            prefix = "uc.%s" % d["name"]
        else:
            if d["name"] == "events":
                names = [m["name"] for m in d["members"] if m["kind"] == "event"]
                out.append("---@alias UcEvent %s\n\n" % "|".join('"%s"' % n for n in names))
                out.append("---Registers a handler for an event.\n---@param event UcEvent\n---@param handler fun(payload: table)\nfunction on(event, handler) end\n\n")
                continue
            out.append("---%s\n%s = {}\n\n" % (d["doc"], d["name"]))
            prefix = d["name"]
        for m in d["members"]:
            if m["kind"] == "function":
                out.append("---%s [%s]\n" % (m["doc"].replace("\n", " "), m["status"]))
                for a in m.get("args", []):
                    out.append("---@param %s%s %s %s\n" % (a["name"], "?" if a.get("optional") else "", lua_type(a["type"]), a["doc"]))
                if m["type"] != "void":
                    out.append("---@return %s\n" % lua_type(m["type"]))
                out.append("function %s.%s(%s) end\n\n" % (prefix, m["name"], ", ".join(a["name"] for a in m.get("args", []))))
            elif m["kind"] == "property":
                out.append("---@type %s\n---%s [%s]\n%s.%s = nil\n\n" % (lua_type(m["type"]), m["doc"].replace("\n", " "), m["status"], prefix, m["name"]))
    return "".join(out).rstrip("\n") + "\n"


# ---- Markdown ---------------------------------------------------------------------------------------------------------

def access_of(m):
    return {"function": "function", "event": "event"}.get(m["kind"], {"r": "read", "w": "write", "rw": "read/write"}.get(m.get("access", ""), ""))


def signature(d, m):
    base = "uc.%s" % d["name"] if d["provider"] == "registry" else d["name"]
    if "scope" in d:
        base = "uc.%s(%s)" % (d["name"], {"slot": "slot", "name": "name", "id": "id"}[d["scope"]])
    if m["kind"] == "function":
        return "%s.%s(%s)" % (base, m["name"], ", ".join(a["name"] for a in m.get("args", [])))
    if m["kind"] == "event":
        return 'on("%s", handler)' % m["name"]
    return "%s.%s" % (base, m["name"])


def render_domain(schema, d):
    out = ["# %s\n\n_Generated by scripts/gen_mod_api.py from modding/api/uc_api.json. Do not edit._\n\n%s\n\n" % (d["name"], d["doc"])]
    out.append("| Member | Kind | Type | Status | Since |\n|---|---|---|---|---|\n")
    for m in d["members"]:
        out.append("| `%s` | %s | `%s` | %s | %s |\n" % (m["name"], access_of(m), m["type"], m["status"], m["since"]))
    for m in d["members"]:
        out.append("\n## `%s`\n\n`%s`" % (m["name"], signature(d, m)))
        if m["kind"] != "event":
            out.append(" returns `%s`" % m["type"] if m["kind"] == "function" else " (`%s`, %s)" % (m["type"], access_of(m)))
        out.append("\n\nStatus: **%s**, since API %s.\n\n%s\n" % (m["status"], m["since"], m["doc"]))
        for a in m.get("args", []):
            out.append("\n- `%s` (`%s`%s): %s" % (a["name"], a["type"], ", optional" if a.get("optional") else "", a["doc"]))
        if m.get("args"):
            out.append("\n")
        ex = m.get("example")
        if ex:
            out.append("\nLua:\n\n```lua\n%s\n```\n\nJavaScript:\n\n```js\n%s\n```\n" % (ex["lua"], ex["js"]))
    return "".join(out)


def render_readme(schema):
    out = ["# Game API reference\n\n_Generated by scripts/gen_mod_api.py from modding/api/uc_api.json. Do not edit._\n\n",
           "API version **%s**. %s\n\n" % (schema["version"], schema["doc"]),
           "Status: `implemented` works in this build, `unverified` works but was not confirmed in every situation, `planned` is in the schema but not available yet (calling it is an error).\n\n",
           "## Domains\n\n| Domain | Provider | Members | Implemented |\n|---|---|---|---|\n"]
    for d in schema["domains"]:
        done = sum(1 for m in d["members"] if m["status"] == "implemented")
        out.append("| [%s](%s.md) | %s | %d | %d |\n" % (d["name"], d["name"], d["provider"], len(d["members"]), done))
    out.append("\n## Hookable functions\n\nUsed with `hook.pre`, `hook.post`, `hook.replace`.\n\n| Name | Guest address | Status | Meaning |\n|---|---|---|---|\n")
    for h in schema.get("hookable", []):
        out.append("| `%s` | `0x%s` | %s | %s |\n" % (h["name"], h["address"], h["status"], h["doc"]))
    return "".join(out)


def render_hookable(schema):
    out = ["// Generated by scripts/gen_mod_api.py from modding/api/uc_api.json. Do not edit.\n",
           "// One line per function that accepts hooks; included with UC_HOOKABLE defined as a macro taking (address, name).\n"]
    for h in sorted(schema.get("hookable", []), key=lambda h: h["address"]):
        out.append('UC_HOOKABLE(0x%su, "%s")\n' % (h["address"], h["name"]))
    return "".join(out)


def render_all(schema):
    files = {"docs/modding/api/README.md": render_readme(schema),
             "docs/modding/uc.d.ts": render_dts(schema),
             "docs/modding/lua/uc.lua": render_lua(schema),
             "modding/generated/hookable.inc": render_hookable(schema)}
    for d in schema["domains"]:
        files["docs/modding/api/%s.md" % d["name"]] = render_domain(schema, d)
    return files


def stale_files(root, files):
    root = pathlib.Path(root)
    stale = []
    for rel, text in files.items():
        path = root / rel
        current = path.read_bytes().decode("utf-8").replace("\r\n", "\n") if path.exists() else None
        if current != text:
            stale.append(rel)
    return stale


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="fail when generated files are out of date")
    root_default = pathlib.Path(__file__).resolve().parent.parent
    parser.add_argument("--root", default=str(root_default), help="repository root (default: this script's repository)")
    parser.add_argument("--schema", help="schema file (default: modding/api/uc_api.json under --root)")
    args = parser.parse_args(argv)
    root = pathlib.Path(args.root)
    schema = load_schema(args.schema or root / "modding" / "api" / "uc_api.json")
    errors = validate(schema)
    if errors:
        print("\n".join("schema error: " + e for e in errors), file=sys.stderr)
        return 1
    files = render_all(schema)
    if args.check:
        stale = stale_files(root, files)
        if stale:
            print("out of date (run python scripts/gen_mod_api.py): " + ", ".join(stale), file=sys.stderr)
            return 1
        return 0
    for rel, text in files.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    print("wrote %d files" % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
