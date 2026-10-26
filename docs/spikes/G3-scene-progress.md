# First scene reachability — primitive construction reached

Native resource playback now reaches730draws/2920vertices in12seconds; see G4-native-resources.md. This supersedes the earlier zero-live-draw observations below. First recognizable game frame remains pending.

2026-10-12. First recognizable game frame remains unverified.

## Confirmed resource content

Native recording generates nonempty resources. Bounded debug capture stores the first four distinct destinations (up to 4096 bytes each), owned under the audit mutex; it runs only with audit enabled. Captures are local diagnostic data, excluded from distributed source/assets.

`tools/spikes/gx_list_audit.py build-trace/run.log` reads those snapshots **outside the game process**. It reports BP/CP/XF/list headers, validates command boundaries and stops before a nonempty primitive's vertex payload. It executes no commands, never mutates graphics state and is not a runtime FIFO renderer. Truncated capture and unsupported opcode are explicit failures.

First four captured resources:

| Destination | Size | Contents |
|---|---:|---|
| 80A15880 | 352 | 62 BP writes, 3 XF writes, zero primitives |
| 80A15A00 | 32 | 1 BP write, NOP padding, zero primitives |
| 80A15A40 | 32 | 1 BP write, NOP padding, zero primitives |
| 80A15A80 | 32 | 1 BP write, NOP padding, zero primitives |

These are first samples, not evidence for every later resource revision. Native mesh/material playback remains required even though these samples contain only state.

## Runtime progress

Twenty-second run: 1200 timer retraces, 1197 native GPU submissions/completions, 2388 nonempty list calls, zero native GXBegin geometry, 13 opens and 15 reads (2254368 bytes). Files include HomeButton resources, biosound.brsar/SNDEFF.dat, three fonts, mes_en.txtres, dialog_en.arc and vib.mot. Timer/GPU progress does not prove scene construction.

Production metadata at 8000002C is now 0x23 (TD-016 closed). DOL `OSGetConsoleType` reads that field; the new run prints `Retail 16`. Geometry and resource-read counts remain unchanged, so this correction is not the scene fix.

Input diagnostic retains the existing no-sample behavior (TD-013): `hle_KPADRead` records arguments then returns zero. Twelve seconds: 1432 polls; first arguments channel0, destination8043A830, maximum16. Caller8003B558 invokes KPADRead at8003B5B4. Its `mulli ...,132` and `memcpy(...,132)` prove this game's sample stride **0x84**, with error byte at sample+0x5D. A generic 0xF0 sample layout would overwrite the wrong fields. A native input bridge needs the remaining accessed fields verified before writing samples.

## Next diagnostic / acceptance limit

Locate scene registration/update conditions and values that suppress draw construction. Existing SDK-signature labels are unreliable; use addresses and actual memory accesses. The guard at8003F74C (r13-0x6C60) protects one-time initialization; it alone does not explain missing geometry. No controller sample and absent native AX/audio callbacks remain service gaps; causality for the empty scene has not been established. No guard bypass, invented input sample or fabricated game image added.

## Native alarm ABI fix — loading now advances

Lifecycle dispatcher8003D1AC/8003D250/8003D2F4/8003D380 walks64 object lists and invokes vtable methods. Opt-in audit now records the DOL-verified call sites and native targets; disabled builds retain the existing early return. Actual calls establish registered scene objects and recurring updates, including80131758 and801152B0; missing scene registration was not the cause.

Eight-second pre-fix run:801161D0 and80102CC8 each228 calls. Tail target800F762C checks slot13 in the128-entry work queue rooted80473BA0. Consumer800F72BC sleeps through OSSleepTicks802626CC between jobs. That function passes OSSetAlarm delay in **r5:r6** and handler in **r7**, with r4 unused. DOL80259644 independently confirms these registers. Periodic variant802596B4 uses start **r5:r6**, period **r7:r8**, handler **r9**. Existing HLE and its original fixtures had shifted all these arguments by one register, preventing timely worker wakeup.

Corrected both native alarm functions; fixtures poison unused r4 and cover one-shot, cancellation, periodic and past-start behavior. RED: actual ABI fixture failed to invoke the alarm. GREEN:50/50 CPU tests. No guest instruction execution or device model added.

Post-fix live run immediately opens **Data/script/r_biouc.arc** (previously absent), then reaches an unregistered indirect callback80112E1C. Verified three-instruction thunk selects80106A04 in r4 and tail-calls8009E450; neighboring functions have the same terminal-branch layout. Explicit static entry added to project/uc.cfg. This proves loading progressed; game-picture acceptance remains open pending the next run.

After that static entry, twelve-second validation run exits0:720 retraces,19 opens,63 reads/**13637056 bytes**,1420 native GPU submissions/completions, zero unmapped MMIO and no VUID/error observed. New files include atract_4_3_en.arc, sysres_g.arc, movie_jimaku.csvtbl, game_en.arc and ef_com.arc. **GXBegin now reaches731 calls**, first primitive0x80/4vertices. They occur during resource recording; live draw/vertex counters remain zero. Scene reachability has advanced to actual primitive construction. Next renderer boundary is native mesh/material playback of these generated resources (TD-014), with transforms/textures/TEV/copies still required. This is not a recognizable game frame.

The embedded alarm examples in the original runtime plan/part2 are historical and use the old incorrect register positions; use the DOL-verified ABI above and current runtime/tests when implementing related calls. Restored plan parts remain preserved.

Release build passes50/50 and stages/copies dist. Direct tool-console startup exposed SDL3.dll+0x1A4699 access violation before guest execution; both release/trace affected. cmd with stderr redirected to a file succeeds. Copied <install dir>/uc.exe started with redirected output from C:/Windows/Temp exits0 after6seconds; normal interactive launch remains unchecked. TD-031 records the unresolved startup boundary; diagnostics report the responsible module/offset. This limit is independent of the verified alarm ABI correction.
