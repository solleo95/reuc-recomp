# Agents

- Live testing of `uc.exe`: use the harness, never system input. Skill: `.claude/skills/uc-test/SKILL.md`; protocol: `docs/testing/test-server.md`.
- Build: `build.bat --no-install` (translator, build, tests, stages `dist`). Tests only: `ctest --test-dir build --output-on-failure`.
- Run a scenario: `cd scripts && python -m uctest run train_gameplay --speed 8`.
- Never run the game against the real saves; the harness copies them into `build/autotest/<run>/saves`.
