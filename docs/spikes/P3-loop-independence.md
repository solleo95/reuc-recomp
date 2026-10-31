# P3: are there loops of the game that can run side by side? (2026-10-31)

Tool: the guest memory tracer (`runtime/mem_trace.{h,cpp}`, build option `UC_TRACE_MEM=ON`, own build folder `build-memtrace`, about 20x slower; leaf functions run as translated code so every access is seen; every `hp()` access of `ppc_rt.h` has a hook, guarded by `tests/test_hp_coverage.py`) and `scripts/loop_independence.py`. Each call of a tracked function records the byte ranges it read and wrote (callees included, its own stack frames left out) and the HLE functions it used; calls with the same caller are compared pairwise. HLE functions cannot be traced inside: the tracer records their pointer arguments (r3..r6, 64 bytes each) as possibly read and written, math HLE functions (`PSMTX*`, `PSVEC*`, `ModelHistoryIdentity`, `Leaf_*`...) are treated as memory-only, GX/`Res*`/video calls make a function `impure`, interrupt masking, mutexes, message queues and the locked cache give `needs-irq`.

Run: first scene, owner's route (menus at x8, then x1), window of 60 VI retraces (one game second), the 15 functions of the update tree from P2 tracked. 28 608 calls recorded.

## Result

| Function | Share of the tracked time | Calls / callers | Verdict | Why |
|---|---|---|---|---|
| `80386840` | 88% | 12 099 calls, all from untracked callers | impure | update and draw are fused per object: `ResMeshDraw`, `GX*`, `Res*Apply` run inside it |
| `8017F3B8` | 14% | 2 096 / 218 (about 10 per caller) | needs-irq, and dependent even without it | `LCStoreBlocks`, `OSLockMutex`; every pair of calls conflicts (10 488 of 10 488) on a global scratch area `8070A0D0-8070A110`, `80709FA0-8070A070` and a word at `8070AA2C` |
| `80163DC4` | 5% | 8 148 / 196 (about 41 per caller) | needs-irq, and dependent even without it | `OSSendMessage` and interrupt masking now and then; every pair conflicts (270 448 of 270 448) on `8070B57C`, `8070B59C-8070B5AC`, `803EDF08-803EDF48` |
| `8003D2F4` | 12% | 16 / 1 | impure | GX draws inside |
| the rest (`80383200`, `8037A5CC`, `80198EB8`, `80186974`, `800FADB4`, `8018A3EC`, `800312CC`, `80066A9C`, `801CC230`, `801AEF8C`, `8037BC0C`) | 2-60% each | exactly one call per caller | single-call | sequential call sites, not loops over objects: nothing to split |

Between the different functions that one caller called, only three pairs conflicted (`800312CC`/`80163DC4` and `80163DC4`/`8037A5CC` on `803EDF08-803EDF48`, `801AEF8C`/`801CC230` on `803E6998-803E69D8`).

## Conclusion

There is no loop in the update tree that the evidence supports running in parallel:

1. The per-object function does the logic and issues the draws in one call, so the draws (order sensitive, recorded into display lists and the GX FIFO) are tied to it.
2. The functions that do repeat per caller communicate through global scratch buffers in `8070xxxx` and the locked cache; every pair of calls conflicts. Making them independent would mean giving each worker its own copy of those buffers, i.e. changing the addresses the translated code uses, call by call, and proving the buffers are written before they are read.
3. The remaining functions are sequential call sites. They would run side by side only as tasks, each worker needing its own register context and guest stack, and the rollback memory (netplay) and the write-barrier epochs are not thread-safe. The largest task is `8037A5CC` (about 21% of the guest thread out of the 23% of its parent), so even perfect overlap of the siblings would save at most the smaller ones, a few percent, for a large and risky change.

So the pool of workers (tasks 17-18 of the plan) is not built: the measurement does not support the gain that would justify it. The tracer stays in the tree behind a build flag for the next time someone wants to check a candidate.
