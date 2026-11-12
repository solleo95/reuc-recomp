# Building from source

**English** · [Русский](building.ru.md) · [Back to the README](../README.md)

You are here: [README](../README.md) › **Building from source**

This page explains how to build the port yourself, run its tests and work on it. If you only want to play, download a [release](https://github.com/solleo95/reuc-recomp/releases/latest) instead: see [Installation](../README.md#installation).

## Contents

- [How the build works](#how-the-build-works)
- [What you need](#what-you-need)
- [Step by step](#step-by-step)
- [Build commands](#build-commands)
- [Tests](#tests)
- [Project layout](#project-layout)
- [Building for Linux (the Steam Deck)](#building-for-linux-the-steam-deck)
- [Building for macOS (Apple Silicon)](#building-for-macos-apple-silicon)
- [Building for Android (AYN Thor)](#building-for-android-ayn-thor)
- [Making a release archive](#making-a-release-archive)
- [Troubleshooting](#troubleshooting)

## How the build works

The port is a *static recompilation*: a translator reads the game's PowerPC program (`main.dol`) and writes C++ source code for it, which is compiled together with the runtime (memory, threads, the Wii SDK replaced by native code), Vulkan and DirectX renderers, the launcher and the modding platform.

Two consequences:

- **You need your own copy of the game** unpacked on the disc: the build reads `game\DATA\sys\main.dol`. Unpack your `RBUE08` (NTSC-U) image with Wit as described in [Installation](../README.md#2-unpack-your-game-image-in-the-launcher-or-with-wit). Other regions do not work.
- The translated code is written to `generated\` (git-ignored). It is derived from the game's code: never commit it, never publish it, never attach it to issues or pull requests.

## What you need

| Tool | Notes |
|---|---|
| Windows 10 / 11, 64-bit | The build runs on Windows; the Linux (Steam Deck) build is cross-built from it, see [Building for Linux](#building-for-linux-the-steam-deck). The macOS and Android builds are made on a Mac: see [macOS](#building-for-macos-apple-silicon) and [Android](#building-for-android-ayn-thor) |
| **Visual Studio Build Tools** (or Visual Studio) with the **"Desktop development with C++"** workload | MSVC runtime, Windows SDK, resource compiler |
| **LLVM** with `clang-cl` | The project is built with `clang-cl` |
| **CMake** 3.25 or newer | |
| **Ninja** | |
| **Python** 3.10 or newer | Build scripts and the test harness (`python` must be on `PATH`) |
| **Vulkan SDK** | `VULKAN_SDK` must be set and the SDK must contain `slangc` (the shader compiler) |
| Internet access during the first configure | CMake downloads the pinned NVIDIA Streamline and AMD FidelityFX SDKs and some libraries (SHA-256 is checked) |
| Your unpacked game | `...\game\DATA\sys\main.dol` |
| A compatible GPU | Vulkan 1.3, DirectX 11.1 with logic operations, or DirectX 12 with Shader Model 6.0 and logic operations; GPU tests require their selected API. See [DirectX support](directx.md). |

`scripts\env.bat` finds all of these (it calls `vcvars64.bat`, looks for `clang-cl`, `cmake`, `ninja` and the Vulkan SDK) and prints what is missing.

## Step by step

1. Clone the repository:

   ```powershell
   git clone https://github.com/solleo95/reuc-recomp.git
   cd reuc-recomp
   ```

2. Create `local.env` from the template and edit it:

   ```powershell
   copy local.env.example local.env
   notepad local.env
   ```

   The only required line is `UC_INSTALL_DIR`: the folder where the game is installed. It must contain `game\DATA\sys\main.dol` (the unpacked disc). The finished build is copied into this folder, next to `game\`, so you can play it at once. `local.env` is git-ignored and holds paths of your machine only. A variable that is already set in the shell wins over the file. The other variables are optional, for development tools; see the comments in [`local.env.example`](../local.env.example).

3. Run the build from a normal Windows terminal (PowerShell or `cmd`):

   ```powershell
   .\build.bat
   ```

   It does six steps: checks the game files, configures and builds the translator, translates the game (cached by a hash of `main.dol`, the translator and the configuration), builds everything and runs the tests, stages the result in `dist\`, and copies `dist\` into `UC_INSTALL_DIR`. The first build takes a long time (translation, ThinLTO link); later builds are incremental.

4. Start `uc_launcher.exe` from the install folder.

## Build commands

| Command | What it does |
|---|---|
| `build.bat` | Full build, tests, stage `dist\`, copy into `UC_INSTALL_DIR` |
| `build.bat --no-install` | The same without the copy (translator, build, tests, stage `dist\`) |
| `scripts\dev.bat` | Build and test in `build\` only (no translation step, no staging) |
| `scripts\bringup.bat [uc.exe arguments]` | A trace build in `build-trace\` and a short run (development) |
| `build-release.bat` | A clean release archive, see [Making a release archive](#making-a-release-archive) |

Build outputs: `build\` (build tree and logs: `configure.log`, `translator_build.log`, `translate.log`), `dist\` (the staged game), `generated\` (translated code). All are git-ignored.

**Logs and debug files.** A build from source writes session logs and crash reports to `logs\` and debug files to `debug\` next to `uc.exe` and `uc_launcher.exe`. An installed release client (the one with `release.json`, the update manifest, next to the executables) writes none of them; it is the same executable, `build-release.bat` needs no second build. A player turns the logs on with the "Enable logs" checkbox on the first launcher tab (`"logs": true` in `config.json`): the launcher passes `UC_LOGS=1` to the game, and the launcher's own log starts from its next launch. A developer can set the environment variable `UC_LOGS=1` before starting the client instead. For a performance complaint the session folder holds `frames.csv` (one row per presented frame: frame time, GPU time per pass, guest threads, disc reads, textures) and `runtime.log`, where `[hitch]` lines name the biggest contributor of every frame much slower than usual (`gpu`, `guest-cpu`, `guest-idle`, `present-wait`, `driver-submit`, `render-thread`, `renderer-waits-guest`, or `unattributed`), `[dvd] open` lines show which stage or movie files the game was loading at that time (every `[hitch]` and `[hitches]` line also ends with the latest `script=`, `room=` and `movie=` opened), `[pipeline]` lines show shader and pipeline compiles on every renderer (Vulkan, DirectX 11 and 12), and once a second `[process]`, `[memory]` and `[vram]` report RAM, CPU load and video memory against its budget.

## Tests

```powershell
ctest --test-dir build --output-on-failure
```

`build.bat` already runs them. They cover the translator, the runtime, the renderer, the launcher, the updater, networking and the modding platform. Some GPU tests need a Vulkan GPU.

**Live tests of the game.** `uc.exe` can be driven by a test harness without any keyboard or mouse input (see [docs/testing/test-server.md](testing/test-server.md)):

```powershell
cd scripts
python -m uctest run train_gameplay --speed 8
```

The harness copies your saves into `build\autotest\<run>\saves` and never plays on the real ones. It always uses a 1280x720 window; do not run tests in fullscreen. For performance changes, compare with the deterministic demo benchmark (`python -m uctest bench`) and not with a single FPS reading; [CONTRIBUTING.md](../CONTRIBUTING.md) describes the rules.

## Project layout

| Folder | Contents |
|---|---|
| `tools/translator`, `common/analyzer`, `common/ppc` | The PowerPC to C++ translator and its analysis |
| `runtime` | The game's runtime: memory, threads, SDK replacements (HLE), audio, DVD file system |
| `renderer` | Vulkan, DirectX 11 and DirectX 12 renderers, upscalers, frame generation, effects, ray tracing |
| `game` | `uc.exe`: startup, window, input, in-game menu |
| `launcher` | `uc_launcher.exe`: settings, bindings, multiplayer lobby, updates |
| `updater` | The isolated updater |
| `modding` | The mod platform: Lua, JavaScript, native DLLs, game API |
| `mods-examples` | Example mods |
| `project` | Translator configuration and signature data |
| `scripts`, `tools` | Build scripts, the test harness (`scripts\uctest`) and development tools |
| `tests` | Unit, integration and end-to-end tests |
| `docs` | Documentation, design notes and measurements |

## What makes the build long, and how it is kept short

A build is limited by its longest file. Three translated functions take 20, 11 and 9 seconds of register allocation each (clang `-ftime-trace`); everything else is below 1.5 seconds per function.
`project/uc.cfg` lists them as `isolate <address>`: the translator puts each in a file of its own and numbers those files first, so the build starts them in the first second. A clean Linux
build of a new translation went from 94 s to 43 s. After the translation changes a lot, `python scripts/compile_hotspots.py` lists the functions that are slow now.

## Compiler cache (ccache)

The translated game is 108 files that take about 25 CPU minutes to compile, so a clean tree or a switch back to an earlier state is slow without a cache. If ccache is installed
(`winget install Ccache.Ccache`), CMake uses it automatically for every build folder (Windows and Linux; `-DUC_CCACHE=OFF` turns it off). The first build of a state fills the cache
and is as slow as before; the same state again (a revert, another folder, a clean build) takes seconds: a full rebuild of the Linux tree measured 19 s instead of minutes.
Check it with `ccache -s`. The settings (`CCACHE_SLOPPINESS=pch_defines,time_macros`, needed for the precompiled headers) travel with the compiler call, so they do not depend on the
shell. On Windows with `clang-cl` a build that uses a shared PDB (`/Zi`) is not cached; `/Z7` is. This has not been tried on the Windows build yet.

## Building for Linux (the Steam Deck)

The Linux build is cross-compiled on Windows (clang against the Steam Runtime "sniper" sysroot) and runs on the Steam Deck. It needs the translated game like the Windows build (`generated\`), plus:

- the sysroot: download `com.valvesoftware.SteamRuntime.Sdk-amd64,i386-sniper-sysroot.tar.gz` from <https://repo.steampowered.com/steamrt3/images/latest-public-stable/> and unpack it with `python scripts/linux/extract_sysroot.py <archive.tar.gz> <out-dir>` (it works without symlink rights);
- LLVM, CMake and Ninja on `PATH` (the same ones as for the Windows build).

```powershell
pwsh scripts/linux/build.ps1 -Target uc_launcher,uc    # build-linux-znver2\ (tuned for the Steam Deck's CPU)
python scripts/linux/deck.py package build/deck-release
python scripts/build_release_deck.py --version X.Y.Z --stage build/deck-release   # writes UC-Linux-X.Y.Z.zip
```

How to install and run it on the Deck: [steamdeck-install.md](steamdeck-install.md). The releasing rules (both archives in every release) are in the same file.

## Building for macOS (Apple Silicon)

The macOS build runs natively on Apple Silicon Macs and draws through Vulkan on MoltenVK. It is built on the Mac itself and was tested on an M4 with macOS 27. It needs the same thing as every build: **your own unpacked `RBUE08` disc** in `UC_INSTALL_DIR/game`.

What you need:

- the Xcode Command Line Tools (Apple clang): `xcode-select --install`;
- from Homebrew: `brew install cmake ninja sdl3 molten-vk vulkan-loader vulkan-headers` (`ccache` is optional) and Python 3;
- `slangc`, the shader compiler: unpack a [Slang release](https://github.com/shader-slang/slang/releases) for macOS arm64 and set `UC_SLANGC` in `local.env` to its `bin/slangc` (or put `slangc` on `PATH`, or set `VULKAN_SDK`). macOS refuses to run a downloaded program until its quarantine flag is removed: `xattr -dr com.apple.quarantine <the unpacked folder>`.
- To unpack the disc on the Mac, use [Wit](https://wit.wiimm.de/) for macOS as in [way B](../README.md#way-b-unpack-by-hand-with-wit). If its arm64 binary is stopped with an invalid signature, sign it for this Mac: `codesign --force -s - wit`.

```bash
cp local.env.example local.env      # set UC_INSTALL_DIR (and UC_SLANGC)
scripts/macos/build.sh
```

`scripts/macos/build.sh` checks the game and the tools, builds the translator, translates `main.dol` (cached like on Windows), builds `uc` in `build-macos/` and installs it into `UC_INSTALL_DIR` with **Umbrella Chronicles.app** (`scripts/macos/make_app.sh`): the program, SDL3, the shaders and the assets go next to `game/`, as `build.bat` installs on Windows. Open the app with a double click or keep it in the Dock. With arguments, `scripts/macos/build.sh <target>...` builds only those CMake targets and installs nothing.

Playing on the Mac: the app starts the game directly (there is no launcher yet), and the settings are in the in-game menu, which opens with **Tab** (Mac keyboards have no Insert key). Gamepads work through SDL. `config.json`, the saves and the logs stay in the install folder. Not on macOS yet: the Wii Remote, the upscalers and ray tracing (MoltenVK has neither `VK_EXT_dynamic_rendering_unused_attachments` nor ray queries), and 8 of the 16 GX logic operations (Metal has none: the 8 that are blending for 0/1 values are drawn with blend factors, the others as a plain copy and named once in the log as `[logic-op]`).

Tests: the POSIX tests are built in `build-macos/tests/linux` (each `test_*` program is one test). The test harness runs on the Mac too: `cd scripts && python3 -m uctest run train_gameplay --exe build-macos/game/uc`.

## Building for Android (AYN Thor)

The Android build runs on arm64 handhelds with Android 13 or newer. It was tested on the AYN Thor (Snapdragon 8 Gen 2, Adreno 740). It is built on a Mac (the script uses the macOS translator; the translated code is the same C++ for every platform), so first set up the [macOS build](#building-for-macos-apple-silicon). Then you need:

- Android Studio's SDK with the **NDK 27.1** (SDK Manager) and `adb`;
- a JDK 17 to 24 for Gradle (Android Studio's own JDK 25 is too new): `brew install --cask temurin@21`;
- the device connected over USB with USB debugging on.

```bash
scripts/android/build.sh                 # build, install the app over adb, copy the program data
scripts/android/build.sh --copy-saves    # also copy UC_INSTALL_DIR/saves when the device has no saves yet
scripts/android/build.sh --no-install    # build the APK only
```

The script builds the game library with the NDK in `build-android/`, the APK with Gradle (`android/`), installs it, and copies the shaders, the program data and the assets into `/sdcard/UmbrellaChronicles`, plus the unpacked game (`game/`, about 3.8 GB) the first time only. That folder is the install folder on the device, laid out like on Windows: `game/`, `data/`, `saves/`, `config.json`, `logs/`, `drivers/`.

On the device open **Umbrella Chronicles** and allow **All files access** (the game, its saves and settings live in that shared folder). The first screen has three buttons, all usable with the gamepad:

- **Play**;
- **Graphics driver**: the system's driver, or a custom Vulkan driver package from `/sdcard/UmbrellaChronicles/drivers` (each press shows the next one);
- **Add a driver (.zip)**: copies a package you pick into that folder.

Driver packages are the AdrenoTools format used by Android emulators: a `.zip` with `meta.json` and the driver library. On the Adreno 740 the choice matters a lot. Qualcomm's own driver crashes in its shader compiler on the game's specialized pipelines, so with it the game draws everything with the general "ubershaders", which is slow in gameplay (about 8 FPS on the Thor at the start of the train mission). **Turnip**, Mesa's open driver, runs the specialized pipelines: with a [Turnip build by MrPurple](https://github.com/MrPurple666/purple-turnip/releases) the same scene runs at about 43 FPS. The first start with a new driver compiles its pipelines (about 35 seconds); later starts take a fraction of a second, because the pipeline cache in `data/` is kept per driver.

Playing: the in-game menu is the handheld one of the Steam Deck and opens with the **View / Select** button. Not on Android yet: the launcher (the settings are in the in-game menu), the Wii Remote, the updater, HDR output; the disc has to be unpacked on a computer.

Logs: the game's output goes to logcat with the tag `uc` (`adb logcat -s uc`) and to the session logs in `/sdcard/UmbrellaChronicles/logs`. For diagnostics the app passes `UC_*` extras to the game as environment variables and `UC_ARGS` as extra arguments: `adb shell am start -n org.reuc_recomp.umbrella_chronicles/.LauncherActivity --es UC_ARGS "--render-scale 1"`.

Tests: the test harness drives the game on the device over adb: `cd scripts && python3 -m uctest run perf --android --skip-tutorials --measure 15`. `UC_ANDROID_DRIVER=<package .zip>` picks the driver of the run (the system's without it); the run folder and its logs are removed from the device afterwards.

## Making a release archive

`build-release.bat` asks for a version `X.Y.Z`, runs `build.bat --no-install`, installs into a clean temporary folder and writes `UC-Windows-X.Y.Z.zip` and `dist-release\` (with `release.json`, the update manifest). It then builds the Steam Deck archive too and writes `UC-Linux-X.Y.Z.zip` (`--skip-linux` for the Windows archive only): `scripts/linux/build.ps1 -ThinLTO` in `build-linux-lto`, with the optimizer profile `project/pgo/uc-linux.profdata` if there is one. **Both archives are always built with ThinLTO** (the script refuses a build without it); record the optimizer profiles first with `pgo.bat` (see [performance-measurement.md](testing/performance-measurement.md), section PGO). The development builds compared during optimisation work have neither ThinLTO nor the profile. Details and the GitHub release steps: [docs/releasing.md](releasing.md) (in Russian).

## Troubleshooting

- **`UC_INSTALL_DIR is not set`** — create `local.env` (step 2) or set the variable in the shell.
- **`main.dol not found`** — `UC_INSTALL_DIR\game\DATA\sys\main.dol` is missing. Unpack the game image there with Wit.
- **`clang-cl not found`, `ninja not found`, `VULKAN_SDK is not set`, `slangc.exe not found`** — install the tool and open a new terminal so that `PATH` and `VULKAN_SDK` are refreshed.
- **Configure fails while downloading SDKs** — check the Internet connection and that no proxy blocks `github.com`; run again.
- **A build or test fails** — read the logs in `build\` (`configure.log`, `translator_build.log`, `translate.log`) and the output of `ctest --output-on-failure`. If you think it is a bug in the project, open an [issue](https://github.com/solleo95/reuc-recomp/issues) with the log, but do not attach game files or the contents of `generated\`.
