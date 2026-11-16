#!/usr/bin/env bash
# Installs the macOS build into the game's folder and writes "Umbrella Chronicles.app" there.   usage: scripts/macos/make_app.sh [install dir]
# The install dir (default UC_INSTALL_DIR) holds game/ and saves/; uc, SDL3, the shaders and the assets go next to them, as build.bat installs
# on Windows: uc keeps everything it reads and writes beside itself (config.json, saves, logs, the pipeline cache in data/, debug screenshots).
# The app is only the launcher with the icon (a double click, the Dock): it starts that uc. build.sh runs this after building uc.
# Needs the build in build-macos and the Homebrew Vulkan loader and MoltenVK the build linked.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/macos/localenv.sh"
fail() { echo "ERROR: $*" >&2; exit 1; }

BUILD="$ROOT/build-macos"
INSTALL="${1:-${UC_INSTALL_DIR:-}}"
[[ -n "$INSTALL" ]] || fail "no install dir: pass one or set UC_INSTALL_DIR in local.env"
INSTALL="$(cd "$INSTALL" && pwd)" || fail "install directory not found: $1"
[[ -f "$INSTALL/game/DATA/sys/main.dol" ]] || fail "$INSTALL has no game/DATA/sys/main.dol (the extracted RBUE08 disc)"
[[ -x "$BUILD/game/uc" ]] || fail "build-macos/game/uc not found: run scripts/macos/build.sh first"

echo "Installing the program into $INSTALL"
install -m 755 "$BUILD/game/uc" "$INSTALL/uc"
install -m 755 "$BUILD/game/libSDL3.0.dylib" "$INSTALL/libSDL3.0.dylib"
# uc finds SDL through @rpath: the build folder's search path becomes the program's own folder. Editing the load commands voids the
# linker's signature, and arm64 runs nothing unsigned, so both files get a new ad-hoc one.
while read -r rpath; do install_name_tool -delete_rpath "$rpath" "$INSTALL/uc"; done < <(otool -l "$INSTALL/uc" | awk '/cmd LC_RPATH/ { getline; getline; print $2 }')
install_name_tool -add_rpath @executable_path "$INSTALL/uc"
codesign --force --sign - "$INSTALL/uc" "$INSTALL/libSDL3.0.dylib" 2>/dev/null
mkdir -p "$INSTALL/data/shaders" "$INSTALL/assets/launcher" "$INSTALL/licenses/SMAA" "$INSTALL/licenses/mimalloc" "$INSTALL/licenses/SDL3"
cp -f "$BUILD"/game/data/shaders/*.spv "$INSTALL/data/shaders/"   # data/pipeline_cache.bin, the GPU's own cache, stays
cp -f "$BUILD/game/data/pipeline_keys.seed" "$ROOT"/project/{identified_sdk,symbols,gx_symbols}.txt "$INSTALL/data/"
cp -f "$ROOT"/assets/launcher/graphite-{train,brand}.png "$INSTALL/assets/launcher/"   # the compile screen's picture
cp -f "$ROOT/THIRD_PARTY_NOTICES.md" "$INSTALL/"
cp -f "$ROOT/third_party/smaa/LICENSE.txt" "$INSTALL/licenses/SMAA/"
cp -f "$BUILD/_deps/mimalloc-src/LICENSE" "$INSTALL/licenses/mimalloc/"
cp -f "$BUILD/_deps/sdl3-src/LICENSE.txt" "$INSTALL/licenses/SDL3/"

APP="$INSTALL/Umbrella Chronicles.app"
echo "Writing $APP"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$ROOT/assets/icon.icns" "$APP/Contents/Resources/AppIcon.icns"
VERSION="$(git -C "$ROOT" describe --always --dirty 2>/dev/null || echo development)"
cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Umbrella Chronicles</string>
  <key>CFBundleDisplayName</key><string>Umbrella Chronicles</string>
  <key>CFBundleIdentifier</key><string>org.reuc-recomp.umbrella-chronicles</string>
  <key>CFBundleExecutable</key><string>Umbrella Chronicles</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>${VERSION}</string>
  <key>CFBundleVersion</key><string>${VERSION}</string>
  <key>LSMinimumSystemVersion</key><string>26.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.action-games</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
EOF
# The launcher: uc next to the app first (the folder can move with it), else the folder the app was made for (the app alone was moved).
cat > "$APP/Contents/MacOS/Umbrella Chronicles" <<EOF
#!/bin/bash
# Umbrella Chronicles: starts uc of the game's folder (written by scripts/macos/make_app.sh).
made_for=$(printf '%q' "$INSTALL")
here="\$(cd "\$(dirname "\$0")/../../.." && pwd)"
arguments=()
for argument in "\$@"; do [[ "\$argument" == -psn_* ]] || arguments+=("\$argument"); done   # Finder's process serial number is not an option of uc
for folder in "\$here" "\$made_for"; do
  if [[ -x "\$folder/uc" ]]; then cd "\$folder" && exec "\$folder/uc" "\${arguments[@]}"; fi
done
osascript -e 'display alert "Umbrella Chronicles" message "The game was not found next to this app. Run scripts/macos/make_app.sh again."' >/dev/null
exit 1
EOF
chmod 755 "$APP/Contents/MacOS/Umbrella Chronicles"
codesign --force --sign - "$APP" 2>/dev/null
touch "$APP"   # Finder and the Dock pick up the new icon
echo "Done: open \"$APP\" (or drag it to the Dock)"
