# Contributing

**English** · [Русский](CONTRIBUTING.ru.md) · [Back to the README](README.md)

Thank you for wanting to help! The project is an early build, still in very active development, and bug reports, test results from other hardware, translations, mods and code are all welcome. This page explains how to work together so that your pull request can be reviewed and merged quickly.

## Contents

- [Ways to help](#ways-to-help)
- [Before you start](#before-you-start)
- [Legal rules (please read)](#legal-rules-please-read)
- [Setting up](#setting-up)
- [Making a change](#making-a-change)
- [Code guidelines](#code-guidelines)
- [Testing](#testing)
- [Performance changes](#performance-changes)
- [Documentation and translations](#documentation-and-translations)
- [Commit messages](#commit-messages)
- [Opening a pull request](#opening-a-pull-request)
- [Review](#review)
- [Security problems](#security-problems)
- [Behaviour](#behaviour)

## Ways to help

- **Test on your hardware** and report what you see ([Issues](https://github.com/solleo95/reuc-recomp/issues), [Discord](https://discord.gg/njy2CqecyY)). The port has been tested only on one PC, so reports from other GPUs, drivers and CPUs are very valuable. Multiplayer needs the most testing.
- **Fix a bug or add a feature** (see below).
- **Translate the launcher.** Its texts are in [`launcher/translations.inc`](launcher/translations.inc) (13 languages); fixes and new languages are welcome.
- **Make mods** and examples ([modding guide](docs/modding/guide/en/README.md), [`mods-examples`](mods-examples)), or improve the modding documentation.
- **Improve the documentation**, in English and in Russian.

## Before you start

- Look through the open [issues](https://github.com/solleo95/reuc-recomp/issues) to see whether the problem is known or already taken.
- For anything bigger than a small fix, **discuss the idea first**: open an issue or ask on [Discord](https://discord.gg/njy2CqecyY). It saves you from writing a change that does not fit the project's direction.
- Keep a pull request to **one topic**. Several small, focused pull requests are easier to review than one large one.

## Legal rules (please read)

- **The project's source code is under [GPL-3.0](LICENSE).** By submitting a pull request you state that you wrote the change (or have the right to submit it) and that you agree to license it under GPL-3.0.
- **Never submit game files or anything derived from them:** no disc images, no extracted textures, models, audio, video or text of the game, no output of the translator (`generated\`), no dumps of the game's memory or code, no decompiled or disassembled game code. This applies to code, tests, fixtures, screenshots with extracted assets, mods and documentation.
- **Never submit proprietary SDK files or copyrighted third-party material:** Nintendo SDK files or leaked source, NVIDIA / AMD binaries other than those already handled by the build, or the DLSS 5 runtime (`nvngx_dlssnr.dll`).
- New third-party code or libraries must have a licence compatible with GPL-3.0. Add them to [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) with the licence text.
- Do not paste personal data, local paths, IP addresses, tokens or saves into issues, logs or code. Replace them with placeholders.

## Setting up

1. Fork the repository and clone your fork.
2. Follow [docs/building.md](docs/building.md): install the tools, unpack **your own** copy of the game, copy `local.env.example` to `local.env`, set `UC_INSTALL_DIR`.
3. Build and run the tests once, before you change anything, so you know the starting state:

   ```powershell
   .\build.bat --no-install
   ```

## Making a change

1. Create a branch from `main`, named after the change (`fix/crash-on-exit`, `feature/hdr-menu`).
2. Make the change. Keep the diff small and focused; do not mix in unrelated refactoring or formatting.
3. Add or update tests and documentation (see below).
4. Build and run **all** tests: `.\build.bat --no-install`. A pull request with failing tests is not merged.
5. If the change is visible or affects the game, try it in the game as well.

## Code guidelines

- **Match the surrounding code**: naming, formatting, comment density and the way errors are handled. The project is C++20 (built with `clang-cl`), with Python for scripts and tools. There is no separate formatter configuration, so follow the file you edit.
- **Comments say why**, not what. A known shortcut gets a comment of the form `shortcut: <the limit>, <when to replace it>`.
- **Errors:** do not swallow them (no empty `catch`); validate everything that comes from outside (files, mods, the network, IPC); do not leak secrets, local paths or personal data into logs; a failure must not leave half-written data.
- **Threads and races:** no unsynchronised shared state; a result of stale asynchronous work must be ignored or cancelled; writes should be idempotent.
- **Do not put machine paths in the code.** Take them from the settings or from `local.env` (see [`local.env.example`](local.env.example)).
- **Netplay and determinism.** The simulation must stay deterministic: do not make game behaviour depend on the wall-clock time, the frame rate or the thread timing. Changes that touch the simulation must keep the checksums of the two players equal ([docs/testing/netplay.md](docs/testing/netplay.md)).
- Avoid new dependencies; if a new one is needed, explain why in the pull request.

## Testing

- Tests live in [`tests`](tests) and run with `ctest --test-dir build --output-on-failure`. New logic (a branch, a parser, a protocol, a calculation) needs a test; a bug fix needs a test that fails before the fix.
- **Live tests of the game** use the harness in [`scripts/uctest`](scripts/uctest) ([docs/testing/test-server.md](docs/testing/test-server.md)), for example `cd scripts` and `python -m uctest run train_gameplay --speed 8`. The harness drives the game without keyboard or mouse input. Do not use real input emulation.
- **Never run tests, benchmarks or demos in fullscreen.** The harness forces a 1280x720 window; keep it. For a higher internal resolution use `--game-arg=--render-scale`.
- **Never run tests against your real saves.** The harness copies the saves into `build\autotest\<run>\saves`.
- Say in the pull request how you tested the change and on which GPU.

## Performance changes

- Judge a change with the **deterministic demo benchmark** (`python -m uctest bench <exe-A> <exe-B> --demo fight2`, see [`.claude/skills/uc-test/SKILL.md`](.claude/skills/uc-test/SKILL.md)), not with a single FPS reading: live runs vary by 7-13%. Use `--runs 3` for small differences.
- Small gains (1-2%) are welcome if the change is correct and not slower elsewhere.
- If the change alters what the game can see, a recorded demo may need to be recorded again (`python -m uctest demo record`, then `demo check`).

## Documentation and translations

- Update the documentation that your change touches. User-facing pages exist in **English and Russian** (the README, building and contributing pages, the modding guide). If you can write only one language, say so in the pull request and a maintainer will help with the other.
- API changes for mods go into the modding documentation and into [`docs/modding/api-changelog.md`](docs/modding/api-changelog.md).
- Launcher texts: add the key to all languages of [`launcher/translations.inc`](launcher/translations.inc). If you do not know a language, put the English text and mention it in the pull request.

## Commit messages

Write a short, specific first line saying **what changed**, and when it helps, **why or what proves it**. Look at `git log` for the style: for example "Collision loop: SegmentTriangleHit continues the triangle loop natively after a miss (differential test: 0 mismatches in 6000 runs)". Use the imperative or the descriptive mood, as the existing history does, and keep unrelated changes in separate commits.

## Opening a pull request

Open the pull request against `main` and fill in the template. It should say:

- **What** changed and **why** (link the issue it fixes: `Fixes #123`);
- **How you tested it**: tests run, live scenarios, your GPU and driver, Windows version;
- **Screenshots or a short video** for any visual change (without game assets that you extracted: screenshots of the running game are fine);
- anything reviewers should look at closely, and any known limits.

Mark the pull request as a **draft** while it is not ready.

## Review

A maintainer reads every pull request. Expect questions and requests for changes: it is normal and not a judgement of you. Please answer in the pull request, push fixes to the same branch and do not close and reopen it. The project is maintained in free time, so a review can take a few days. If a pull request is quiet for a long time, a friendly ping is fine.

## Security problems

If you find a vulnerability (for example in the updater, the mod loader or the network code), please **do not open a public issue**. Use GitHub's private vulnerability reporting on the repository's *Security* tab or write to the maintainer privately on [Discord](https://discord.gg/njy2CqecyY).

## Behaviour

Be kind and respectful, assume good intentions and keep discussions about the work. Harassment and insults are not accepted; the maintainers may remove comments and block people who break this rule.
