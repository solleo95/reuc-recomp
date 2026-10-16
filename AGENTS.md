# Agents

- Live testing of `uc.exe`: use the harness, never system input. Skill: `.claude/skills/uc-test/SKILL.md`; protocol: `docs/testing/test-server.md`.
- Build: `build.bat --no-install` (translator, build, tests, stages `dist`). Tests only: `ctest --test-dir build --output-on-failure`.
- Run a scenario: `cd scripts && python -m uctest run train_gameplay --speed 8`.
- Never run the game against the real saves; the harness copies them into `build/autotest/<run>/saves`.
- Performance changes: judge them with the deterministic demo bench, never with a single FPS run (live fight runs vary 7-13%). `python -m uctest bench <exe-A> <exe-B> --demo fight2` (one round screens differences above ~5% (the machine drifts by up to 6% between runs); `--runs 3` alternates the order A B, B A, A B and resolves about 2%) replays the recorded fight (identical guest work, run-to-run sd about 1%) and prints a paired 95% CI; stage the variants as separate exes next to `dist` data (e.g. `dist/uc_before.exe`), A is the baseline. Record a demo once with `python -m uctest demo record <name>` and again after a change that alters what the guest can see (`demo check <name>` proves replay == recording). Absolute FPS is confirmed afterwards with a live `uctest run fight` at speed 1. Tests run muted (`--mute` is automatic in the harness). Details: `.claude/skills/uc-test/SKILL.md`.
