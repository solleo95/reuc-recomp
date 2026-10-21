import importlib.util
import re
import sys
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(sys.argv.pop(1))
sys.path.insert(0, str(ROOT / "tools"))


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


hooks = load("pacing_hooks")
audit = load("timing_audit")


class PacingHooksTest(unittest.TestCase):
    def test_hle_map_and_tables_agree(self):
        declared = set(re.findall(r"void hle_(\w+)\(ppc::Ctx& c\);", (ROOT / "game/game_pacing.h").read_text(encoding="utf-8")))
        mapped = hooks.hle_map_hooks(ROOT / "project/hle_map.txt", declared)
        missing = sorted(set(mapped.values()) - set(hooks.HOOKS))
        self.assertEqual(missing, [], "pacing hooks bound in hle_map.txt but absent from HOOKS")
        self.assertTrue(set(hooks.BODY_HOOKS.values()) <= set(hooks.HOOKS))
        self.assertEqual(mapped.get("80165610"), "CrossfadeStep")

    def test_timing_audit_uses_shared_tables(self):
        self.assertIs(audit.HOOKS, hooks.HOOKS)
        self.assertIs(audit.BODY_HOOKS, hooks.BODY_HOOKS)


if __name__ == "__main__":
    unittest.main()
