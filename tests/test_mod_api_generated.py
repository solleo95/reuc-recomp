"""The mod API schema: validation rules, rendered output, and that the committed generated files match the schema."""
import copy
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
sys.path.insert(0, str(root / "scripts"))
import gen_mod_api as gen  # noqa: E402

schema = gen.load_schema(root / "modding" / "api" / "uc_api.json")


def check(condition, message):
    if not condition:
        print("FAIL:", message)
        sys.exit(1)


def broken(mutate):
    copy_ = copy.deepcopy(schema)
    mutate(copy_)
    return gen.validate(copy_)


def member(s, domain, name):
    return next(m for d in s["domains"] if d["name"] == domain for m in d["members"] if m["name"] == name)


# The real schema is valid.
check(gen.validate(schema) == [], "real schema has errors: %s" % gen.validate(schema))

# Validation rules.
check(broken(lambda s: member(s, "mem", "read_u8").update(status="maybe")), "unknown status accepted")
check(broken(lambda s: member(s, "mem", "read_u8").update(kind="widget")), "unknown kind accepted")
check(broken(lambda s: member(s, "mem", "read_u8").update(since="2.0")), "since newer than the API version accepted")
check(broken(lambda s: s["domains"][0]["members"].append(dict(s["domains"][0]["members"][0]))), "duplicate member name accepted")
check(broken(lambda s: s["hookable"].append({"name": "Bad", "address": "XYZ", "status": "planned", "doc": "x"})), "bad hookable address accepted")
check(broken(lambda s: s["hookable"].append(dict(s["hookable"][0]))), "duplicate hookable accepted")
check(broken(lambda s: member(s, "mem", "read_u8").update(status="implemented") or member(s, "mem", "read_u8").pop("example")), "implemented member without example accepted")
check(broken(lambda s: member(s, "player", "magazine").pop("access")), "property without access accepted")

# Rendering.
files = gen.render_all(schema)
check("modding/generated/hookable.inc" in files and "docs/modding/uc.d.ts" in files and "docs/modding/lua/uc.lua" in files, "outputs missing")
check("docs/modding/api/player.md" in files and "docs/modding/api/README.md" in files, "domain pages missing")
dts = files["docs/modding/uc.d.ts"]
check("function read_u8(address: number): number;" in dts, "d.ts: mem.read_u8 missing")
check("function player(slot: 1 | 2): PlayerHandle;" in dts, "d.ts: player(slot) missing")
check("magazine: number;" in dts and "readonly present: boolean;" in dts, "d.ts: player properties wrong")
check('function on(event: "on_step", handler: (payload: any) => void): void;' in dts, "d.ts: on() overload missing")
lua = files["docs/modding/lua/uc.lua"]
check("function uc.mem.read_u8(address) end" in lua and "---@field magazine integer" in lua, "lua stubs wrong")
inc = files["modding/generated/hookable.inc"]
check('UC_HOOKABLE(0x8006C268u, "GameWeaponUpdate")' in inc, "hookable.inc wrong")
page = files["docs/modding/api/player.md"]
check("`magazine`" in page and "implemented" in page and "uc.player(1).magazine = 15" in page, "player page wrong")
check("planned" in files["docs/modding/api/input.md"], "planned members must show up as planned (input.fire_held)")

# The committed files are current.
stale = gen.stale_files(root, files)
check(not stale, "generated files out of date, run: python scripts/gen_mod_api.py (%s)" % ", ".join(stale))
print("ok")
