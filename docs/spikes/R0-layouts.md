# R0 — Guest structure layouts and low-memory globals

Date: 2026-10-12. Tools: `tools/xref --disasm`. Every offset below is proven by a disassembly line of the translated SDK code in `main.dol` (RVL_SDK build `0x4199_60831`).

## OSThread (decimal offsets as printed by the disassembler, hex in brackets)

Evidence: `OSCreateThread` (`80261940`).

| Field | Offset | Evidence |
|---|---|---|
| context.gpr[1] (initial SP) | `0x04` | stack top rounded to 8 minus 8 is stored in the context |
| context.gpr[3] (entry argument) | `0x0C` | `stw r26,12(r31)` with r26 = `param` (r5) |
| state (u16) | 712 (`0x2C8`) | `sth r7,712(r3)` with r7 = 1 (ready) |
| attr (u16) | 714 (`0x2CA`) | `sth r0,714(r3)` with r0 = `attr & 1` (detached bit) |
| suspend count | 716 (`0x2CC`) | `stw r7,716(r3)` with r7 = 1 |
| priority | 720 (`0x2D0`) | `stw r8,720(r3)` |
| base priority | 724 (`0x2D4`) | `stw r8,724(r3)` |
| exit value (`val`) | 728 (`0x2D8`) | `stw r29,728(r3)` with r29 = -1 |
| queue / link / join queue | 744–760 (`0x2E8`–`0x2F8`) | zeroed |
| stack base (top) | 772 (`0x304`) | `stw r27,772(r31)` (r27 = `stack`, r6) |
| stack end (lowest address) | 776 (`0x308`) | `stw r4,776(r31)` with r4 = `stack - size`; `stw 0xDEADBABE,0(r4)` is the canary |
| error | 780 (`0x30C`) | zeroed |
| specific[] | 784, 788 (`0x310`, `0x314`) | zeroed |
| priority range check | — | `cmpi r8,0 / cmpi r8,31`: valid priorities 0..31 |

Prototype confirmed: `OSCreateThread(thread r3, func r4, param r5, stack r6, stack_size r7, priority r8, attr r9)`; the new thread's `r2`/`r13` are copied from the creator by `__OSContextInit` (called through `8025AE90`).
The values in `runtime/hle/os_layout.h` match this table.

## Low memory (addresses seen as `lis rX,0x8000 / 0xC000` + offset in the game and SDK code)

| Offset | Reader (example) | Meaning | Value provided |
|---|---|---|---|
| `0x00–0x1F` | `__start` `8000405C`, game | disc header: game id `RBUE`, maker `08`, version | first 32 bytes of `boot.bin` |
| `0x28` | `OSInit` | MEM1 size | `0x01800000` |
| `0x30` / `0x34` | `OSInit`, arena code | arena low / high | after BSS rounded to 32 / `0x81700000` |
| `0x38` | `8025D0C8` | FST pointer (DVD) | 0 (DVD is replaced) |
| `0xCC` | 9 sites | video mode (0 NTSC) | 0 |
| `0xE4` | 7 sites (`80261398`) | **current thread pointer** (`OSGetCurrentThread` reads it) | maintained by the scheduler switch hook |
| `0xF8` | **75 sites** (`800F7154` …) | bus clock | `243000000` |
| `0xFC` | OS | CPU clock | `729000000` |
| `0xC4`, `0xD4`, `0xD8`, `0xDC`, `0xE0`, `0xF4`, `0x60` | `OSInit`, `OSThread` code | OS-owned lists and flags written by the OS itself | 0 (the SDK initialises them) |
| `0x3100` | `OSGetPhysicalMem1Size` (`8025F480`) | MEM1 physical size | `0x01800000` |
| `0x3104` | OS | MEM1 simulated size | `0x01800000` |
| `0x310C`, `0x3110` | `OSInit+252` | MEM1 arena low/high override; `0` means "use defaults" | 0 |
| `0x3118` | `OSGetPhysicalMem2Size` (`8025F48C`) | MEM2 physical size | `0x04000000` |
| `0x311C` | OS | MEM2 simulated size | `0x04000000` |
| `0x3124`, `0x3128` | `OSInit+424…548` | MEM2 arena low/high override; `0` skips the branch | 0 |
| `0x3130`, `0x3134` | `__OSInitIPCBuffer` (`80262DDC`) | IPC buffer low/high copied into SDA variables | 0 (IOS IPC is not used by the runtime) |
| `0x3138` | `BATConfig` (`8025FA94`) | Hollywood/BAT configuration flag | 0 (`BATConfig` is a `Nop`) |
| `0x3140`, `0x3144` | `OSInit` `80258A00`, `__OSFPRInit` | IOS version and build date (read through the uncached mirror `0xC0003140`) | 0 (only printed/compared) |
| `0x30C4…0x30F4`, `0x315C…0x3184` | OS startup | boot flags, simulated memory and loader data | 0 until a trace shows a reader needs a value |

Rule: low memory starts as all zero; `init_lowmem` writes only the rows with a value above. If a bring-up trace shows a reader of another word, add the value there with its evidence, and a row here.

## DVDFileInfo / DVDCommandBlock

Evidence: `DVDReadAsyncPrio` (`80266638`).

| Field | Offset | Evidence |
|---|---|---|
| command block state | `0x0C` | SDK convention; set to 0 (END) on open and completion, 1 while busy (not read by the checks below) |
| start address | 48 (`0x30`) | `lwz r6,48(r26)` (disc offset >> 2 in the SDK; the runtime stores the entry number) |
| length | 52 (`0x34`) | `lwz r0,52(r3)` |
| callback | `0x38` | SDK convention |

Bounds check in `DVDReadAsyncPrio`: accepted when `offset <= length` and `offset + len < length + 32` (`add. r4,r29,r28 … addi r0,r3,32 … cmpl r4,r0`). The runtime applies the same rule (plan Task 6).

## Not needed yet

`OSAlarm`, `OSMutex`, `OSMessageQueue`: the HLE keeps their state host-side keyed by guest address and never reads or writes the guest fields, so their layouts are not required (the SDK initialiser functions that the game calls are HLE'd too). Revisit if the game is found to read their fields directly.

## DefaultThread (added by the final review)

`__OSThreadInit` (`80261104`) builds `r30 = 0x805F88D8` (`lis 0x8060; addi -0x7728`), fills state 2, attr 1, priority and base priority 16, val -1, and stores it at `0x800000E4` (`802611B8 stw r30,228(r29)`).
The boot code runs on this OSThread, so the scheduler's main thread is registered under `0x805F88D8`.

Low-memory words written by `init_lowmem` since the bring-up (rows above that say 0 are superseded): `0x3124` = `0x90000800`, `0x3128` = `0x933E0000` (MEM2 arena, `OSInit` +424…548), `0x3130` = `0x933E0000`, `0x3134` = `0x93400000` (IPC buffer, `__OSInitIPCBuffer` `80262DDC`).
