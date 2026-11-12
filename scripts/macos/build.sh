#!/usr/bin/env bash
# Native macOS (Apple Silicon) build: the translator, the translation of main.dol, then the game.   usage: scripts/macos/build.sh [cmake target...]
# The macOS counterpart of build.bat and scripts/translate.bat. Without targets it builds uc and installs it into UC_INSTALL_DIR with
# "Umbrella Chronicles.app" (scripts/macos/make_app.sh), as build.bat copies dist there. Logs go to build-macos/.
# Needs: Homebrew cmake, ninja, sdl3, molten-vk, vulkan-loader, vulkan-headers (ccache optional), Python 3, and slangc (UC_SLANGC in local.env,
# slangc on PATH, or the Vulkan SDK's when VULKAN_SDK is set).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$ROOT/build-macos"
GEN="$ROOT/generated"
TARGETS=("$@")
INSTALL_APP=0
[[ ${#TARGETS[@]} -gt 0 ]] || { TARGETS=(uc); INSTALL_APP=1; }

source "$ROOT/scripts/macos/localenv.sh"

fail() { echo "ERROR: $*" >&2; exit 1; }

echo "[1/5] Checking game files and tools"
[[ -n "${UC_INSTALL_DIR:-}" ]] || fail "UC_INSTALL_DIR is not set. Copy local.env.example to local.env and set it to the folder with game/DATA/sys/main.dol (docs/building.md)."
[[ -d "$UC_INSTALL_DIR" ]] || fail "install directory not found: $UC_INSTALL_DIR"
GAME_DOL="${UC_GAME_DIR:-$UC_INSTALL_DIR/game}/DATA/sys/main.dol"
[[ -f "$GAME_DOL" ]] || fail "main.dol not found: $GAME_DOL"
SLANGC="${UC_SLANGC:-$(command -v slangc || true)}"
[[ -z "$SLANGC" && -n "${VULKAN_SDK:-}" ]] && SLANGC="$VULKAN_SDK/bin/slangc"
[[ -x "$SLANGC" ]] || fail "slangc not found: set UC_SLANGC in local.env to an unpacked Slang release's bin/slangc"
for tool in cmake ninja python3; do command -v "$tool" >/dev/null || fail "$tool not found (brew install $tool)"; done
mkdir -p "$BUILD"

configure() {
  cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release --toolchain "$ROOT/cmake/toolchains/macos-arm64.cmake" "-DUC_SLANGC=$SLANGC" \
    > "$BUILD/configure.log" 2>&1 || { tail -30 "$BUILD/configure.log"; fail "configure failed (see $BUILD/configure.log)"; }
}

echo "[2/5] Configure and build the translator"
# modding/generated/hookable.inc comes from modding/api/uc_api.json; .gitignore's "generated/" keeps it out of the repository, so a fresh clone has none
[[ -f "$ROOT/modding/generated/hookable.inc" ]] || python3 -I "$ROOT/scripts/gen_mod_api.py" >/dev/null || fail "gen_mod_api.py failed"
configure
cmake --build "$BUILD" --target translator > "$BUILD/translator_build.log" 2>&1 || { tail -30 "$BUILD/translator_build.log"; fail "translator build failed (see $BUILD/translator_build.log)"; }

echo "[3/5] Translating the game (cached by DOL + translator + uc.cfg + hle.cfg hash)"
export GAME_DOL TRANSLATOR="$BUILD/tools/translator/translator" CFG="$ROOT/project/uc.cfg" HLECFG="$ROOT/project/hle.cfg"
python3 -I "$ROOT/tools/hle_cfg.py" "$ROOT/project" || fail "hle.cfg generation failed"
KEY="$(python3 -I "$ROOT/tools/translate_key.py")" || fail "could not hash inputs for the translate cache"
if [[ -f "$GEN/game_sources.cmake" ]] && grep -qxF "# translation-input: $KEY" "$GEN/game_sources.cmake"; then
  echo "generated/ is up to date"
else
  # the translator keeps every earlier generation (~50 MB each); only the one in game_sources.cmake is used
  "$TRANSLATOR" --dol "$GAME_DOL" --cfg "$CFG" --cfg "$HLECFG" --out "$GEN" --shard 150 --cache-key "$KEY" > "$BUILD/translate.log" 2>&1 \
    || { cat "$BUILD/translate.log"; fail "translation failed"; }
  cat "$BUILD/translate.log"
fi
python3 -I "$ROOT/tools/prune_generations.py" "$GEN"

echo "[4/5] Building ${TARGETS[*]}"
configure   # picks up generated/game_sources.cmake
cmake --build "$BUILD" --target "${TARGETS[@]}" 2>&1 | tee "$BUILD/build.log" || fail "build failed (see $BUILD/build.log)"   # pipefail: the build's status

if [[ $INSTALL_APP -eq 1 ]]; then
  echo "[5/5] Installing into $UC_INSTALL_DIR"
  "$ROOT/scripts/macos/make_app.sh" "$UC_INSTALL_DIR"
else
  echo "[5/5] Skipping the install (targets given)"
fi
echo "Done: $BUILD"
