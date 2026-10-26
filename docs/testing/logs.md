# Automatic diagnostics

Each launcher and game process creates a separate directory beside its executable:
`logs/<launcher-or-game>-<UTC-date-time>-<PID>-<sequence>/`.
The launcher's `runtime.log` records child PIDs, so its GPU probe and game can be matched to their sessions.

- `runtime.log`: timestamped stdout/stderr from the application and its libraries; Windows/CPU/RAM and build identity; graphics device/driver; startup and shader compilation progress; UI/backend errors and slow frames; NAT, lobby, tunnel traffic/RTT/loss, rollback and disconnects; periodic CPU/GPU/audio/queue statistics; normal exit status.
- `frames.csv`: each presented game frame, with CPU/GPU timings, event work, queue depth, memory/texture work, disk reads and audio counters. Written asynchronously every second and on normal exit. The in-memory queue is bounded; dropped rows and write failures are reported.
- `mods.log`: mod messages when present, with the existing per-mod limit.
- `crashes/`: existing native/guest crash report and memory bundle when the game's crash handler runs. Forced termination and power loss cannot produce a crash report.

For a startup/UI/network issue, reproduce it once, close normally if possible, and share the corresponding launcher and game session directories from **both players**. Keep `data/pipeline_cache.bin`: deleting it forces fresh shader compilation. For an unresolved freeze, copy the current logs before terminating the process; readers can open `runtime.log` while it is being written.

Console output and the test harness remain intact. Logs contain local paths and network endpoints, and frame logs/crash memory bundles can grow large. They are not uploaded automatically. An unwritable logs directory is reported on the inherited error stream; logging failure does not prevent startup. `UC_FRAME_LOG` retains its explicit CSV path override.

Verification: `ctest --test-dir build -R test_diagnostic_log --output-on-failure`; launcher `--smoke`; from `scripts`, `python -m uctest.netplay_startup` exercises a late peer and cancellation using isolated saves and windowed games.
