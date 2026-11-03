#!/bin/sh
# Starts the game on the Steam Deck. The launcher runs this file when Play is pressed (uc-launcher.sh runs it directly for a test run). It lives next to `uc`, `data/`
# and `game/`. Fullscreen at the display's size; any argument is passed on to `uc` (a later --window-mode wins). UC_GAME_ARGS: extra arguments before them.
cd "$(dirname "$0")" || exit 1
env | grep -E '^(SDL|STEAM|SteamGame|GAMESCOPE)' | sort > steam-env.txt   # what Steam hands the game (diagnostics for the gamepad/gyro)
sdl_minor=-1   # the system's SDL3 (the Deck has 3.4 with Wayland) wins when it is 3.2 or newer; else the copy in lib/ (built with X11 only) is used
for f in /usr/lib/libSDL3.so.0.* /usr/lib64/libSDL3.so.0.* /usr/lib/x86_64-linux-gnu/libSDL3.so.0.* /usr/local/lib/libSDL3.so.0.*; do
  [ -e "$f" ] || continue
  v=${f##*libSDL3.so.0.}
  [ "${v%%.*}" -gt "$sdl_minor" ] 2>/dev/null && sdl_minor=${v%%.*}
done
[ "$sdl_minor" -ge 2 ] || export LD_LIBRARY_PATH="$PWD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
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
