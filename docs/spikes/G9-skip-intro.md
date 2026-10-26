# Native startup-screen option

2026-10-12. User requested a test option skipping Nintendo safety screens, CAPCOM and CRIWARE logos; future launcher label **«Пропустить вступительные ролики»**. CLI: `uc.exe --skip-intro`. Default false. Later story movies and save/error dialogs retain their ordinary flow.

## Verified boundary

DOL80245E90 loads `logo.csv` and52-byte logo rows. The total display duration is row+48. Startup80246130 starts the logo object at804D1E6C via80245C34. Object fields: +4active,+5loaded,+28complete,+24phase,+34row index,+38row count,+3Crows. Update8024585C returns early until active/loaded; its phase4 branch advances a completed row, sets +28 on the final row and performs normal Nintendo display-state cleanup.

Native IntroLogoUpdate wraps only8024585C. With the option enabled and this exact startup object ready, it selects the final row, phase4 and its validated total duration. The original statically compiled update performs completion. The wrapper lowers the original first stack store; explicit entry80245860 retains the rest of the compiled body, including LR/stack restoration. With the option disabled, all original instructions still execute. No runtime opcode parsing, JIT, elapsed-time manipulation or global movie bypass added.

## Validation

Parser and native-boundary tests first fail for missing interfaces, then pass. Tests cover default false/flag true, disabled behavior, another object instance, not-yet-loaded startup data, the original stack/LR convention and malformed row memory.

Real trace:12seconds, `--skip-intro --validation --gx-audit --log-dvd`, exit0,719retraces,14312draws/57248vertices,179copies/presents, zero unmappedMMIO. One explicit skip event precedes later resource loading; inspected GPU readback reaches the same `Unknown error occurred.` screen within this bounded run. Previously the startup logos required roughly44seconds to reach the first post-logo trap. This comparison verifies the option's practical purpose, not a controlled performance benchmark. One scoped Luna review reports no issues.

## Next blocker

The game's `r_biouc.lub` startup script names `_ExecLogo`, `_Fade2In`, `_ExecSaveFileCheck`, then `opening_movie`. The message resource identifies `Unknown error occurred.` as ID178, next to save/load messages. Current IOS filesystem opens return−6; NAND initialization802AC778 calls ISFS_OpenLib802A7534 and returns an error if it fails.

Runtime now confirms the failing gate: `[save] NANDGetHomeDir caller80134D80 initialization0 (requires2)`. DOL802ACAC4 reads the SDK initialization word at r13−25056, requires2, and returns−128 otherwise. Game save check80134D60 calls this function at80134D80; on failure it writes internal status13 at object+8. The controller at80133428 routes that status through its default error action. The failing native filesystem initialization therefore prevents even obtaining the game's save home directory. Native save-file service is required; returning a fabricated success or skipping the dialog would retain a false save state.

Next implementation must replace the verified NAND/ISFS file-service boundaries with host files, preserve missing-file results, capacity/status checks and asynchronous callback ownership, and keep all host paths under a dedicated save root. Use current CLI skip for repeated short checks. Movie playback remains a later dependency.

Trace diagnostics now capture verified resource/dialog boundaries and caller addresses.8011637C loads common assets and80116674 initializes startup resources; their logs are labelled `[startup resource]`. Entering these functions alone does not prove an error. Do not use automatic signature names as semantic proof.

Release build passes52/52tests, stages dist and installs to <install dir>. Installed EXE from C:/Windows/Temp:12seconds with skip exits0,719retraces,14110draws,178presents and one skip event;6seconds without skip exits0,361retraces,364draws,350presents and no skip event. Both runs join workers with zero unmappedMMIO/ordinary-validation errors. Full movie/gameplay and unchanged default startup beyond that bounded6-second run remain unverified.
