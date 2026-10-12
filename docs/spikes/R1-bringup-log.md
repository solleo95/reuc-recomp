# R1 — Bring-up log (runtime plan, Task 9)

Goal (M1): `scripts\bringup.bat --seconds 20 --log-dvd` exits 0, `retraces >= 1000`, `dvd opens >= 1`, no trap, fatal, deadlock or no-progress report.
Result: **reached** after 17 iterations. Last run: `retraces 1198, dvd opens 13, dvd reads 15 (2254368 bytes), unmapped mmio 0`, exit 0.
Every change followed the nativity rule; emulation-like stopgaps are in `docs/tech-debt.md` (TD-010 … TD-016).

| # | Symptom | Cause | Change | Nativity |
|---|---------|-------|--------|----------|
| 0 | uc.exe died with no output (0xC0000409) | `path::string()` threw on two junk `.lnk` file names in the game tree | crash/terminate handlers; `DvdFs` stores UTF-8 names (test with a Japanese name) | native |
| 1 | `guest trap … sc` in `DCFlushRange`; 50 unmapped MMIO reads at `0xCD006xxx` | cache maintenance uses `sc`; the DI/SI/EXI/AI blocks also appear at `0xCD006000` | `DC*/IC*/LC*` maintenance functions → `Nop` (no cache to flush); same register blocks mapped at the second window; `DCZeroRange` and `LCStore*` stay translated | native, no debt |
| 2 | spin in `EXISync` | EXI transfer-complete bit never clears | EXI driver replaced at call level (`hle/exi.cpp`): no device attached | **TD-010** |
| 3 | spin in `__OSInitAudioSystem` | waits for the DSP reset bit / boot mailbox | → `Nop` | **TD-011** |
| 4 | `rfi` unsupported in `BATConfig` | MMU BAT setup | `__OSInitMemoryProtection` → `Nop` (flat address space) | native, no debt |
| 5 | `ClearMEM2Arena` wrote 4 GB; "Not enough IPC arena" | `0x3124/0x3128` (MEM2 arena) and `0x3130/0x3134` (IPC buffer) were zero | `init_lowmem` writes them (test-first); values from the `OSInit`/`__OSInitIPCBuffer` code | native |
| 6 | indirect call to unknown address `8003FA80` | a destructor pointer built with `lis/addi` was not a known entry | analyzer phase 3: `lis/addi` text pointers right after a terminator are entries (test-first) | native |
| 7 | deadlock in `IOS_Open` | IPC reply never comes | IOS call interface replaced at call level (`hle/ios.cpp`), logs `[ios] open <path>` | **TD-012** |
| 8 | indirect call to unknown address `80049250` | constructor-list entry without a prologue after a plain branch | analyzer: a run of code pointers in data is a function table (test-first) | native |
| 9 | `sc` in `80257E38` | `PPCSync` (`sc; blr`) | symbol added with evidence; → `Nop` | native, no debt |
| 10 | deadlock reported while a thread slept with interrupts off | the idle loop ignored timers when the sleeper had interrupts off | scheduler keeps interrupt state per thread; idle runs with interrupts on (test-first) | native |
| 11 | thread asleep in `GXDrawDone` | waits for the GPU finish interrupt | `GXDrawDone` → `Nop` | **TD-014** |
| 12 | `KPADInit` spun on WUD status | Bluetooth stack never starts | `KPADInit`/`KPADEnableMotor` → `Nop`, `KPADRead` → `Return0` | **TD-013** |
| 13 | host AV in an IOS callback (write to guest `0xBFFF98C8`) | the interrupt context had `r13 = 0` | interrupt context copies `r2/r13` of the interrupted thread (test-first) | native |
| 14 | spin in `__AI_SRC_INIT` | calibrates the AI sample counter against the timebase | → `Nop` | TD-011 |
| 15 | spin in `AXInit` after `DSPAddTask` | waits for the AX task's init callback | `DSPInit` → `Nop`; `DSPAddTask` accepts the task and delivers its init callback in interrupt context (`hle/dsp.cpp`, test-first) | **TD-011** |
| 16 | spin loops never saw interrupt-delivered flags | translated code had no safe points | `ppc::poll` on every backward branch (emitter test-first) | **TD-015** |

Tooling added on the way: `--log-sleep` (guest trace + stack back chain at every `OSSleepThread`), thread dump and interrupt state in the timed-exit report, guest address decoding in the host crash report.

## Open observations

- `OSGetConsoleType` reports a development console (`0x10000002`), TD-016.
- The ring buffer prints oldest first; function names from the signature database are often wrong in the middle of a function (labels are `nearest symbol + offset`), TD-007.
- After M1 the game opens `Data/HomeButton2/*`, `Data/sound/*`, fonts, `mes_en.txtres`, `dialog_en.arc`, `vib.mot` and keeps running; no frame is drawn yet.
