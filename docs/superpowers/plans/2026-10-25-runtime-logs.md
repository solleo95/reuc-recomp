# Runtime diagnostics

- Preserve console and harness output while teeing stdout/stderr to a unique session under `logs`.
- Record OS/CPU/RAM/build identity, startup phases, GPU/driver/cache information, launcher stalls and network transitions.
- Enable the existing asynchronous frame CSV; keep crash reports and mod logs in the session directory.
- Test output preservation, concurrent writers, Unicode paths, normal teardown and unavailable log storage; then run launcher smoke and delayed network startup.
- Publish matching launcher/game binaries. The friend's popup delay remains unconfirmed until their logs are available.

Completed: 187/187 CTest checks; GUI smoke with inherited handles and without a console; delayed-peer/cancellation live test with default session CSV and runtime-log assertions. Core and final review found no actionable issues. Both EXEs deployed with matching SHA256 to dist-raw, dist-raw-copy and <install dir>. Existing archive was not rebuilt.
