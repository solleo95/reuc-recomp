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
- [Making a release archive](#making-a-release-archive)
- [Troubleshooting](#troubleshooting)

## How the build works

The port is a *static recompilation*: a translator reads the game's PowerPC program (`main.dol`) and writes C++ source code for it, which is compiled together with the runtime (memory, threads, the Wii SDK replaced by native code), Vulkan and DirectX renderers, the launcher and the modding platform.

Two consequences:

- **You need your own copy of the game** unpacked on the disc: the build reads `game\DATA\sys\main.dol`. Unpack your `RBUE08` (NTSC-U) image with Wit as described in [Installation](../README.md#2-unpack-your-game-image-with-wit). Other regions do not work.
- The translated code is written to `generated\` (git-ignored). It is derived from the game's code: never commit it, never publish it, never attach it to issues or pull requests.

## What you need

| Tool | Notes |
|---|---|
| Windows 10 / 11, 64-bit | The port is Windows-only |
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

**Logs and debug files.** A build from source writes session logs and crash reports to `logs\` and debug files to `debug\` next to `uc.exe` and `uc_launcher.exe`. An installed release client (the one with `release.json`, the update manifest, next to the executables) writes none of them; it is the same executable, `build-release.bat` needs no second build. To get the logs from a release client anyway (for example to debug an update), set the environment variable `UC_LOGS=1` before starting it.

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

## Making a release archive

`build-release.bat` asks for a version `X.Y.Z`, runs `build.bat --no-install`, installs into a clean temporary folder and writes `UC-Windows-X.Y.Z.zip` and `dist-release\` (with `release.json`, the update manifest). Details and the GitHub release steps: [docs/releasing.md](releasing.md) (in Russian).

## Troubleshooting

- **`UC_INSTALL_DIR is not set`** — create `local.env` (step 2) or set the variable in the shell.
- **`main.dol not found`** — `UC_INSTALL_DIR\game\DATA\sys\main.dol` is missing. Unpack the game image there with Wit.
- **`clang-cl not found`, `ninja not found`, `VULKAN_SDK is not set`, `slangc.exe not found`** — install the tool and open a new terminal so that `PATH` and `VULKAN_SDK` are refreshed.
- **Configure fails while downloading SDKs** — check the Internet connection and that no proxy blocks `github.com`; run again.
- **A build or test fails** — read the logs in `build\` (`configure.log`, `translator_build.log`, `translate.log`) and the output of `ctest --output-on-failure`. If you think it is a bug in the project, open an [issue](https://github.com/solleo95/reuc-recomp/issues) with the log, but do not attach game files or the contents of `generated\`.
