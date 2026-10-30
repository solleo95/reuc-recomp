#!/bin/sh
# Starts the game on the Steam Deck: add this file to Steam as a non-Steam game (Desktop Mode: Games > Add a Non-Steam Game > Browse, show All files),
# or run it in a terminal. It lives next to `uc`, `data/` and `game/` (scripts/linux/deck.py stage copies it).
# Fullscreen at the display's size by default; any argument after the script's own is passed on (a later --window-mode wins).
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
exec "${UC_EXE:-./uc}" --window-mode borderless --skip-intro "$@"
