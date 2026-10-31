#!/bin/sh
# Starts the game on the Steam Deck. The launcher runs this file when Play is pressed (uc-deck.sh runs it directly for a test run). It lives next to `uc`, `data/`
# and `game/`. Fullscreen at the display's size; any argument is passed on to `uc` (a later --window-mode wins). UC_GAME_ARGS: extra arguments before them.
cd "$(dirname "$0")" || exit 1
env | grep -E '^(SDL|STEAM|SteamGame|GAMESCOPE)' | sort > steam-env.txt   # what Steam hands the game (diagnostics for the gamepad/gyro)
unset SDL_GAMECONTROLLER_IGNORE_DEVICES   # Steam lists the Deck's own controller (28de:1205) there to hide it from SDL games, which leaves only a gyro-less evdev copy
export SDL_JOYSTICK_HIDAPI_STEAMDECK=1   # read the Deck's controller (and its gyro) directly through SDL's HIDAPI driver
export SDL_JOYSTICK_HIDAPI=1
# A test run started from the PC (scripts/uctest/deck.py, UC_DECK_GAMEMODE=1) leaves next-run.sh: exports for the run, `set --` with the game's arguments and an `exec` redirect
# of the output; it is used once, so a later start from the Game Mode list is a normal start.
if [ -f next-run.sh ]; then
  . ./next-run.sh
  rm -f next-run.sh
fi
# shellcheck disable=SC2086  # UC_GAME_ARGS is a list of arguments on purpose
exec "${UC_EXE:-./uc}" --window-mode borderless $UC_GAME_ARGS "$@"
