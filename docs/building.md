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
| Windows 10 / 11, 64-bit | The build runs on Windows; the Linux (Steam Deck) build is cross-built from it, see [Building for Linux](#building-for-linux-the-steam-deck) |
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

## Making a release archive

`build-release.bat` asks for a version `X.Y.Z`, runs `build.bat --no-install`, installs into a clean temporary folder and writes `UC-Windows-X.Y.Z.zip` and `dist-release\` (with `release.json`, the update manifest). It then builds the Steam Deck archive too and writes `UC-Linux-X.Y.Z.zip` (`--skip-linux` for the Windows archive only): `scripts/linux/build.ps1 -ThinLTO` in `build-linux-lto`, with the optimizer profile `project/pgo/uc-linux.profdata` if there is one. **Both archives are always built with ThinLTO** (the script refuses a build without it); record the optimizer profiles first with `pgo.bat` (see [performance-measurement.md](testing/performance-measurement.md), section PGO). The development builds compared during optimisation work have neither ThinLTO nor the profile. Details and the GitHub release steps: [docs/releasing.md](releasing.md) (in Russian).

## Troubleshooting

- **`UC_INSTALL_DIR is not set`** — create `local.env` (step 2) or set the variable in the shell.
- **`main.dol not found`** — `UC_INSTALL_DIR\game\DATA\sys\main.dol` is missing. Unpack the game image there with Wit.
- **`clang-cl not found`, `ninja not found`, `VULKAN_SDK is not set`, `slangc.exe not found`** — install the tool and open a new terminal so that `PATH` and `VULKAN_SDK` are refreshed.
- **Configure fails while downloading SDKs** — check the Internet connection and that no proxy blocks `github.com`; run again.
- **A build or test fails** — read the logs in `build\` (`configure.log`, `translator_build.log`, `translate.log`) and the output of `ctest --output-on-failure`. If you think it is a bug in the project, open an [issue](https://github.com/solleo95/reuc-recomp/issues) with the log, but do not attach game files or the contents of `generated\`.
