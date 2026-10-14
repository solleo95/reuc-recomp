import importlib.util
import pathlib
import sys

spec = importlib.util.spec_from_file_location("train_route", pathlib.Path(sys.argv[1]) / "scripts/train_route.py")
route = importlib.util.module_from_spec(spec)
spec.loader.exec_module(route)

accepted = "\n".join(f"[native-action] {name} object 80010000 accepted 1" for name in ["Reload", "Grenade", "Knife", "Knife"])
route.require_native_actions(accepted)
route.require_native_actions("[native-action] Reload object 80010000 accepted 1",cleared=True)
try:
    route.require_native_actions(accepted,cleared=True)
except RuntimeError:
    pass
else:
    raise AssertionError("Cleared grenade/knife must stay inactive")
assert route.tutorial_accepts_input("[native-tutorial] 20000000")
assert not route.tutorial_accepts_input("[native-tutorial] 20000000\n[native-tutorial] 40000000")
assert not route.tutorial_accepts_input("[native-tutorial] 00100000")
assert not route.tutorial_accepts_input("")
assert route.latest_context("") == "inactive"
assert route.latest_context("[native-context] gameplay actor 1\n[native-context] qte actor 1") == "qte"
assert route.latest_context("[native-context] gameplay actor 1\n[native-context] menu actor 1") == "menu"
assert route.gameplay_rates("[native-context] menu\n[perf] presents 60\n[native-context] gameplay\n[perf] presents 30\n[perf] presents 29\n[native-context] menu\n[perf] presents 60") == [29]
assert route.gameplay_rates("[perf] presents 60") == []
assert route.gameplay_rates("[native-context] gameplay\ndebug screenshot: frame.png\n[perf] presents 7\n[perf] presents 30") == [30]
for invalid in ["", accepted.replace("Grenade object", "Unknown object"), accepted.replace("accepted 1", "accepted 0"), accepted + "\nfatal: crash"]:
    try:
        route.require_native_actions(invalid)
    except RuntimeError:
        pass
    else:
        raise AssertionError("Incomplete or crashed route must not pass")
