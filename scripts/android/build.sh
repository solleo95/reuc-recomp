#!/usr/bin/env bash
# Android build for arm64 handhelds (the AYN Thor): the game's libraries with the NDK, the APK with Gradle, the install on the device over adb.
# usage: scripts/android/build.sh [--no-install] [--copy-saves]
#   --no-install   build the APK only
#   --copy-saves   also copy UC_INSTALL_DIR/saves to the device when it has none yet
# The game lives in /sdcard/UmbrellaChronicles on the device, laid out as UC_INSTALL_DIR on the Mac: game/ (the extracted disc, copied once),
# data/ (shaders, symbol lists), assets/, and what the game writes there (saves, config.json, logs). The translation is the macOS translator's
# (scripts/macos/build.sh translator): the generated code is the same C++ for every platform.
# Needs: what scripts/macos/build.sh needs, the Android SDK and NDK (Android Studio), adb, and the device with USB debugging on.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/scripts/macos/localenv.sh"
fail() { echo "ERROR: $*" >&2; exit 1; }

INSTALL_DEVICE=1 COPY_SAVES=0
for argument in "$@"; do
  case "$argument" in
    --no-install) INSTALL_DEVICE=0 ;;
    --copy-saves) COPY_SAVES=1 ;;
    *) fail "unknown argument: $argument" ;;
  esac
done
BUILD="$ROOT/build-android"
DEVICE_DIR=/sdcard/UmbrellaChronicles
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
NDK="${UC_ANDROID_NDK:-${ANDROID_NDK_HOME:-$(ls -d "$ANDROID_HOME"/ndk/* 2>/dev/null | sort -V | tail -1)}}"
[[ -d "$NDK" ]] || fail "Android NDK not found: install it with Android Studio's SDK Manager or set UC_ANDROID_NDK"
gradle_java() {   # Gradle 8.14 runs on JDK 17..24; java_home answers with the newest JDK when the asked one is missing, so its answer is checked
  local version home
  for version in 21 22 24 17; do
    home="$(/usr/libexec/java_home -v "$version" 2>/dev/null)" || continue
    [[ "$("$home/bin/java" -version 2>&1 | head -1)" =~ \"$version[.\"] ]] && { echo "$home"; return; }
  done
}
export JAVA_HOME="${UC_ANDROID_JAVA_HOME:-$(gradle_java)}"
[[ -x "${JAVA_HOME:-}/bin/java" ]] || fail "no JDK 17..24 for Gradle: install one (brew install --cask temurin@21) or set UC_ANDROID_JAVA_HOME"

echo "[1/5] Translating the game (the macOS translator)"
"$ROOT/scripts/macos/build.sh" translator > /dev/null
SLANGC="${UC_SLANGC:-$(command -v slangc || true)}"

echo "[2/5] Building the game's libraries for Android (arm64)"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release --toolchain "$ROOT/cmake/toolchains/android-arm64.cmake" "-DUC_ANDROID_NDK=$NDK" "-DUC_SLANGC=$SLANGC" \
  > "$BUILD.configure.log" 2>&1 || { tail -30 "$BUILD.configure.log"; fail "configure failed (see $BUILD.configure.log)"; }
cmake --build "$BUILD" --target uc uc_cli main_hook file_redirect_hook gsl_alloc_hook hook_impl 2>&1 | tee "$BUILD/build.log" | grep -E 'error|FAILED|Linking' || true
[[ -f "$BUILD/game/libmain.so" ]] || fail "build failed (see $BUILD/build.log)"
JNI="$BUILD/android-jniLibs/arm64-v8a"   # android/app/build.gradle.kts packages these (and strips them)
mkdir -p "$JNI"
rm -f "$JNI/libc++_shared.so"   # libc++ is linked into libmain.so (cmake/toolchains/android-arm64.cmake)
cp -f "$BUILD/game/libmain.so" "$BUILD/game/libSDL3.so" "$JNI/"
cp -f "$BUILD"/_deps/adrenotools-build/src/hook/lib{main_hook,file_redirect_hook,gsl_alloc_hook,hook_impl}.so "$JNI/"   # libadrenotools' hooks (custom drivers)

echo "[3/5] Packaging the APK"
VERSION="$(git -C "$ROOT" describe --always --dirty 2>/dev/null || echo development)"
(cd "$ROOT/android" && ./gradlew --quiet assembleDebug "-PucNativeBuild=$BUILD" "-PucVersion=$VERSION") || fail "Gradle failed"
APK="$ROOT/android/app/build/outputs/apk/debug/app-debug.apk"
echo "APK: $APK"
[[ $INSTALL_DEVICE -eq 1 ]] || { echo "Done (not installed: --no-install)"; exit 0; }

echo "[4/5] Installing the app"
[[ "$(adb get-state 2>/dev/null)" == "device" ]] || fail "no device: connect it over USB with USB debugging on, then accept this Mac on its screen (adb devices)"
adb install -r "$APK" > /dev/null

echo "[5/5] Copying the program data to $DEVICE_DIR"
adb shell mkdir -p "$DEVICE_DIR/data/shaders" "$DEVICE_DIR/assets/launcher"
adb push "$BUILD"/renderer/shaders/*.spv "$DEVICE_DIR/data/shaders/" > /dev/null
adb push "$ROOT/project/pipeline_keys.seed" "$ROOT/project/identified_sdk.txt" "$ROOT/project/symbols.txt" "$ROOT/project/gx_symbols.txt" "$DEVICE_DIR/data/" > /dev/null
adb push "$ROOT/assets/launcher/graphite-train.png" "$ROOT/assets/launcher/graphite-brand.png" "$DEVICE_DIR/assets/launcher/" > /dev/null
if ! adb shell test -f "$DEVICE_DIR/game/DATA/sys/main.dol"; then
  GAME="${UC_GAME_DIR:-${UC_INSTALL_DIR:-}/game}"
  [[ -f "$GAME/DATA/sys/main.dol" ]] || fail "the device has no game yet and $GAME has none to copy (set UC_INSTALL_DIR)"
  echo "Copying the extracted disc (about 4 GB, once)"
  adb push "$GAME" "$DEVICE_DIR/" | tail -1
fi
if [[ $COPY_SAVES -eq 1 ]] && ! adb shell test -d "$DEVICE_DIR/saves" && [[ -d "${UC_INSTALL_DIR:-}/saves" ]]; then
  echo "Copying the saves of $UC_INSTALL_DIR"
  adb push "$UC_INSTALL_DIR/saves" "$DEVICE_DIR/" | tail -1
fi
echo "Done: start Umbrella Chronicles on the device (or: adb shell am start -n org.reuc_recomp.umbrella_chronicles/.LauncherActivity)"
