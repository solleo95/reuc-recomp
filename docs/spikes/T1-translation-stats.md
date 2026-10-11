# T1 — Translating the real `main.dol`

Date: 2026-10-12. Tool: `translator` (`build.bat` step 3). Config: `project/uc.cfg`.

## Final numbers

| Item | Value |
|---|---|
| Guest instructions translated | 968,536 (all of text 0 + text 1) |
| Functions (primary entries) | 14,620 |
| Secondary entries (branch into the middle of a function) | 149 |
| Fall-through tail calls (function end runs into the next entry) | 49 |
| Jump tables resolved | 234 (231 detected + 3 added to `uc.cfg`) |
| Indirect tail calls (`bctr`, mostly virtual-call thunks) | 490 |
| Unresolved switch `bctr` | 0 |
| Branches to an address inside no function | 0 |
| Illegal/data words (emitted as traps) | 1,648 (1,634 zero words + 14 data words in text) |
| OE-form arithmetic (not implemented, traps) | 0 |
| Unsupported privileged instructions (traps) | 73 (`mtsr`, `mfsr`, `tlbie` …, expected in OS/boot code that is replaced by HLE) |
| Generated source | 49 MB in 100 files (150 functions per file) |
| Clean compile of `uc_game` (clang-cl `/O2`, x86-64-v3, 32 threads) | **19 s** (0.31 min), `uc_game.lib` 50 MB |
| `uc_smoke` on the real DOL | links; entry `8000403C` is a known function |
| Translation time | 0.6 s |

## Triage that shaped the translator

First run: 16,554 functions, 1,302 "branch into the middle" traps, 525 "unresolved" `bctr`.

1. **Branches into the middle of a function** were shared tails (for example `_restgpr`-style epilogues with 130 references each). Splitting the function would break loops that cross the split (host recursion), so the analyzer records the target as a *secondary entry* of its owner; the emitter keeps one body with a `switch` at the top and one stub `fn_<owner>_<target>` per extra entry.
2. **"Unresolved" `bctr`** were almost all virtual-call thunks (`lwz r12,0(r3); lwz r12,N(r12); mtctr r12; bctr`): indirect tail calls, not switches. Switch shape now requires `cmplwi` + `lwzx` + `mtctr`.
3. **Cascade of false entries**: data words that point at case labels (from switch tables that were not found yet) became function entries and cut big functions, so the next table failed its range check too. Fixes: a data pointer after a plain `b` needs a prologue; data pointers into the body of a function that holds an unresolved switch are ignored; table bases kept in another register set far above (`addi r3,r29,lo`) are followed.
4. **Stale shards**: a run that produced fewer files than the previous one left old `game_NNN.cpp` files, giving duplicate symbols. The translator now removes `game_*.cpp|h` in its output directory first (test `translator_cleans_stale`).
5. Three switches whose table base is a register set by the caller (`r23`, `r25`, `r18`) were resolved by scanning the data sections for pointer runs into the function and are listed in `project/uc.cfg`.

## Verdict: PASS

Translation and compilation succeed, no switch is unresolved and no branch leaves the known functions. Compile time is far below the 15-minute bound (19 s).

The 73 unsupported privileged instructions were located: 24 sit in the `.init` vector area, 10 in functions named from `os.a`, and 39 in other functions, almost all around `0x8025xxxx` (the OS code area). Two functions, `0x802A26F4` and `0x802A3EB0`, are not yet attributed to a library. They are probably TRK/OS code, but that is not shown: check them when the OS layer is written.

## Open items

- **Loops across primary-entry boundaries** would recurse on the host stack (a primary entry that is jumped to backwards from a later function). The tool does not report them yet; the runtime plan should add a counter or a debug assert in `call_indirect`/tail calls and run the game's main loop before trusting this.
- Functions: 14,620 versus 9,029 `bl` targets in S1; the difference comes from data-pointer and prologue entries. Only a few functions were spot-checked; no systematic audit was done. A comparison with Ghidra or decomp-toolkit would be a cheap cross-check if problems appear (see `docs/decisions.md`).
- The review also noted (not fixed here): scalar single-precision ops now write both paired-single halves, but FP rounding details (SNaN quieting, `fctiw` rounding mode, double rounding in `fmadds`/`ps_madd`) differ slightly from hardware.
- Real run-time behaviour is untested: nothing here executes the game.
