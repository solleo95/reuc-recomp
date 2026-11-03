#!/bin/sh
# The Linux entry point (on the Steam Deck: add this file to Steam as a non-Steam game, Desktop Mode: Games > Add a Non-Steam Game > Browse, show All files), or run it in a
# terminal. It lives next to `uc_launcher`, `uc.sh`, `uc`, `data/` and `game/`. It starts the launcher; the launcher starts the game through uc.sh.
cd "$(dirname "$0")" || exit 1
unset SDL_GAMECONTROLLER_IGNORE_DEVICES   # Steam lists the Deck's own controller (28de:1205) there to hide it from SDL games, which leaves only a gyro-less evdev copy
export SDL_JOYSTICK_HIDAPI_STEAMDECK=1    # read the Deck's controller (and its gyro) directly through SDL's HIDAPI driver
export SDL_JOYSTICK_HIDAPI=1
# A test run started from the PC (scripts/uctest/deck.py, UC_DECK_GAMEMODE=1) leaves next-run.sh: it goes straight to the game, without the launcher, as before.
if [ -f next-run.sh ]; then
  UC_GAME_ARGS=--skip-intro exec ./uc.sh "$@"
fi
exec ./uc_launcher "$@"
