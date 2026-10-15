"""Screens and menu targets of RBUE08, identified from game memory (no timers).

Signature keys: an int is a `ui` slot of the frame event, "mN" is menu word N (page-alive words read from the guest,
see InputProbeSnapshot::menu_pages). Values: an int must match exactly, "nz" means non-zero.
Measured 2026-10-15 (docs/spikes/G22-autoplay-memory.md): each menu page creates its object ~45-90 ticks after the
press that opens it and sets its word; presses before that are dropped, so a page counts as reached only when its
word is set. Moving backwards is not modelled (words are never cleared by this harness).
"""
from dataclasses import dataclass
from typing import Optional


@dataclass(frozen=True)
class Screen:
    name: str
    signature: dict


@dataclass(frozen=True)
class Target:
    screen: str                  # screen the target is pressed on
    x: float                     # aim coordinates in [-1,1]; screen x = 640+800*x, y = 360+600*y at 1280x720
    y: float
    next: str                    # screen that must replace `screen` after the press
    retry_ticks: int = 30        # game ticks (1/60 s) after which a press without effect is repeated; a repeat during the
                                 # leave transition is harmless because the next page does not exist yet (45-120 ticks)


_MODE = {1: 4}
SCREENS = {s.name: s for s in (
    Screen("game_over", {4: 7}),                                  # "You died - Continue? Yes/No"
    Screen("mission", {2: 5, 3: 5, 6: 6}),                        # mission loading, tutorial pages and gameplay HUD
    Screen("title", {0: 2}),                                      # "press A" screen after the opening movies
    Screen("save_select", {5: 3}),
    Screen("start_dialog", {**_MODE, "m3": "nz"}),                # "The game starts now. Ready?"
    Screen("loadout", {**_MODE, "m2": "nz", "m3": 0}),
    Screen("character", {**_MODE, "m1": "nz", "m2": 0, "m3": 0}),
    Screen("chapter", {**_MODE, "m0": "nz", "m1": 0, "m2": 0, "m3": 0}),
    Screen("scenario", {**_MODE, "m0": 0, "m1": 0, "m2": 0, "m3": 0}),
    Screen("chapter_done", {1: 9, "m0": "nz"}),                   # chapter select after a finished chapter (page words stay set from the way in)
)}

TARGETS = {
    "title_start": Target("title", 0.0, 0.0, "save_select"),
    "save_slot_1": Target("save_select", -0.425, -0.05, "scenario"),
    "scenario_train": Target("scenario", -0.5125, -0.0333, "chapter"),
    "chapter_1": Target("chapter", -0.325, -0.2, "character"),
    "chapter_2": Target("chapter", 0.025, -0.2, "character"),             # card right of chapter 1; used by --chapter 2 from a save where it is unlocked (verified)
    "chapter_3": Target("chapter", -0.206, 0.18, "character"),            # chapters 3 and 4: positions read from a screenshot of the empty cards, never pressed
    "chapter_4": Target("chapter", 0.244, 0.167, "character"),
    "character_rebecca": Target("character", -0.24, -0.067, "loadout"),
    "loadout_confirm": Target("loadout", 0.625, 0.4, "start_dialog"),
    "start_yes": Target("start_dialog", -0.35, 0.2, "mission"),
    "game_over_continue": Target("game_over", 0.22, 0.325, "mission"),   # "Continue? Yes": pixel (815,555) of the Game Over screen
}


def _matches(frame: dict, key, value) -> bool:
    if isinstance(key, int):
        words = frame.get("ui") or []
        have = words[key] if key < len(words) else None
    else:
        words = frame.get("menu") or []
        index = int(key[1:])
        have = words[index] if index < len(words) else None
    if have is None:
        return False
    return have != 0 if value == "nz" else have == value


def screen_of(frame: dict) -> Optional[str]:
    for screen in SCREENS.values():
        if all(_matches(frame, key, value) for key, value in screen.signature.items()):
            return screen.name
    return None
