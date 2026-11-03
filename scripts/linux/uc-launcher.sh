#!/bin/sh
# The Linux entry point (on the Steam Deck: add this file to Steam as a non-Steam game, Desktop Mode: Games > Add a Non-Steam Game > Browse, show All files), or run it in a
# terminal. It lives next to `uc_launcher`, `uc.sh`, `uc`, `data/` and `game/`. It starts the launcher; the launcher starts the game through uc.sh.
cd "$(dirname "$0")" || exit 1
sdl_minor=-1   # the system's SDL3 (the Deck has 3.4 with Wayland) wins when it is 3.2 or newer; else the copy in lib/ (built with X11 only) is used
for f in /usr/lib/libSDL3.so.0.* /usr/lib64/libSDL3.so.0.* /usr/lib/x86_64-linux-gnu/libSDL3.so.0.* /usr/local/lib/libSDL3.so.0.*; do
  [ -e "$f" ] || continue
  v=${f##*libSDL3.so.0.}
  [ "${v%%.*}" -gt "$sdl_minor" ] 2>/dev/null && sdl_minor=${v%%.*}
done
[ "$sdl_minor" -ge 2 ] || export LD_LIBRARY_PATH="$PWD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
unset SDL_GAMECONTROLLER_IGNORE_DEVICES   # Steam lists the Deck's own controller (28de:1205) there to hide it from SDL games, which leaves only a gyro-less evdev copy
export SDL_JOYSTICK_HIDAPI_STEAMDECK=1    # read the Deck's controller (and its gyro) directly through SDL's HIDAPI driver
export SDL_JOYSTICK_HIDAPI=1
# A test run started from the PC (scripts/uctest/deck.py, UC_DECK_GAMEMODE=1) leaves next-run.sh: it goes straight to the game, without the launcher, as before.
if [ -f next-run.sh ]; then
  UC_GAME_ARGS=--skip-intro exec ./uc.sh "$@"
fi
exec ./uc_launcher "$@"
