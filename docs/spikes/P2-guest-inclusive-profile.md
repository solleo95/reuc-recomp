# P2: where the guest thread's time goes, with call stacks (2026-10-31)

Tool: `UC_CPU_SAMPLE=1 UC_CPU_SAMPLE_STACKS=1 UC_CPU_SAMPLE_SECONDS=9` on the Deck (Game Mode), `scripts/inclusive_profile.py <samples> <binary>`. Each row is the share of the guest thread's samples that had the function anywhere on the stack (1 kHz process-CPU sampling, about 7200 samples per scene). Owner's settings, first scene (67.9 FPS with the sampler on, no visible overhead) and the `fight` scenario at its saved point.

The `fight` scenario's saved slowdown point (F5 mark) lands at a quiet moment (5 actors, 76 FPS), not at the heavy leech wave. Both profiles agree within 1.5 points on every row, so the structure below is stable; the heavy wave was not profiled (the owner asked for no long fight runs).

## The call tree (first scene, share of the guest thread)

| Inclusive | Function (guest address) | What is known |
|---|---|---|
| 71% | `body_8003F674` | the frame, under `hle_GameLoop` (67%) |
| 47% | `body_8000403C` -> 46% `body_80386840` | per-frame dispatcher |
| 23% | `body_80383200` | children: `8037A5CC` 21%, `80198EB8` 12%, `80186974` 12%, `800FADB4` 11%, `8018A3EC` 10%, `8017F3B8` 9.5%, `800312CC` 6%, `fn_80180554` 3.4%, `80163DC4` 3.3% |
| 15% | `body_8037BC0C` | the draw traversal: children `8037B678` 15%, `80378090` 15%, `80376D54` 11%, and it holds `hle_ResMeshDraw` |
| 24% | `body_8003A46C` -> `8003D2F4` 15.6% -> `80066A9C` 15.8% | children `801CC230` 8.5%, `801AEF8C` 4.7%, `80215CD8` 1.9% |
| 11.3% | `hle_AXFrame` | only calls guest code (`call_guest`): the game's own sound engine callback, run from a 3 ms timer; subtree `803976AC` 6.9%, `803AC124` 5.8%, `80275F34` 3.6%, `803B2B34` 3.1%, `80398BF8` 2.4% |
| 12.2% | `hle_ResMeshDraw` | GX side of the draws (with `GXCallDisplayList` 4.8%, `GXBegin` 3.4%: about 20% GX in total) |

Runtime leaves are small: `gx_fifo_write` 2.4%, `hle_Leaf_80394374` 2.3%, `write_attribute` 1.8%, `hle_PSMTXConcat` 1.2%, `Sched::safe_point` 11.6% inclusive (the other guest threads' work reached through the baton, mostly the AX callback).

## Corrections

- `hle_AXFrame` is **not** absent from gameplay: its self time is 0.2%, so a flat profile hid it, but its inclusive cost is 11% of the guest thread. It is the game's sound engine (the 0x8039xxxx-0x803Bxxxx code looks like the audio middleware, not game logic) running in a timer callback, so it cannot be moved to another thread as it is.
- No loop has been identified by name yet. Reading the translated source of the nodes above is the next step; their addresses are not in `project/symbols.txt` because there is no behavioural evidence yet.

## Candidates

1. **Sound engine subtree under `hle_AXFrame` (11%).** Self-contained library code with its own state; a native replacement of the library (voice allocation, envelopes, sequencer) would remove 11% of the guest thread, but it is big and must reproduce the library's writes to AX voice memory exactly. Alternative: keep it as is.
2. **Per-object update tree under `body_80383200` (23%).** Children are siblings called in sequence: likely update passes over many objects; independence must be proven with the tracer (Task 15).
3. **Draw traversal `body_8037BC0C` (15%).** Order-sensitive (draws are recorded in order into display lists and the GX FIFO); not a parallel candidate, but the native-mesh path under it is already mostly HLE.
4. **`body_80066A9C` subtree (16%).** Unnamed; `801CC230` (8.5%) is the largest child.
5. Micro-wins (1-2% each) are the leaves above; none exceeds 2.4% self.

## The sound engine subtree in detail (owner chose it, 2026-10-31)

`hle_AXFrame` (11% of the guest thread, every 3 ms) calls three things in guest code: the AX library's PB sync `body_80275F34(0x5F50)` and `80273688`, then the game's registered AX callback (r13-25636), which is the sound middleware (looks like MusyX: its API and its frame callback guard shared state with `OSDisableInterrupts`/`OSRestoreInterrupts` pairs). Self time inside the subtree (share of the whole guest thread, first scene / fight): `80275F34` 2.7 / 2.4%, `8039A328` 1.5 / 0.9%, `803B2B34` 0.7 / 0.7%, `80398BF8` 0.7 / 0.6%, `8027C430` 0.6%, `803AC124` 0.5 / 0.7%, `802759E4` 0.5 / 0.5%, `80398184` 0.4 / 0.6%: eight functions, about 7.5% together, none above 2.7%.

- `80275F34` is the AX library's voice parameter-block sync (loop over the AXVPB array, 600-byte entries, writes the 320-byte PB fields that `mix_ax_voice` reads). Non-leaf (calls `80275870`, `80274344`, `80273674` and SaveGpr/RestoreGpr).
- The middleware functions call only `hle_OSDisableInterrupts` / `hle_OSRestoreInterrupts` (and each other). `tools/leaf_hle.py` / `leaf_hybrid.py` reject any function with a call, so they cannot convert these; a hand port or an extension of the generator is needed.
- Running the callback on another thread is not viable: it is serialised against the game's API calls by interrupt masking, which the scheduler models as "the callback runs at a safe point on the one guest thread"; making it concurrent needs those critical sections to become locks and the determinism of demos/netplay depends on the current order.

Ceiling: about 3% (the AX sync) from one hand port plus about 1-2% from `8039A328`; the rest are 0.5% functions.

## Option 2 (wider leaf generator) against option 3 (parallel loops), measured 2026-10-31

Guest-thread self time of the translated functions by kind of calls (first scene / fight; `S:/TEMP/claude/classify.py` over the generated sources):

| Kind | First scene | Fight | Top entries |
|---|---|---|---|
| calls other guest code | 35.9% | 37.0% | 80275F34 2.7%, 8037AB90 1.3% |
| leaf (no calls), still translated | 11.0% | 12.1% | 802647AC 0.9%, 80235294 0.6-1.0%, 80369E68 0.5%, 80078A00 0.3-0.4% |
| calls only HLE (not interrupts/frame) | 4.7% | 4.7% | 800A4434 1.6%, 803949B8 0.9% |
| interrupt-only calls | 1.5% | 0.9% | 8039A328 |
| leaf + SaveGpr/RestoreGpr | 1.4% | 1.3% | 80137F78 1.1% |
| already native HLE | 19.1% | 19.0% | |

Why the 11% of plain leaves are not native yet (`tools/leaf_hle.py` rejection reasons): `bdnz` counter loops (802647AC), a conditional `blr` return (80235294), `ps_nmsub` (80369E68, 80369BCC), `stmw` (80078A00), a call (803B1378). All four are missing generator features, not hard functions: the hybrid generator runs register-only instructions through the unchanged `ppc::op_*`, so exactness is by construction and the differential test exists.

Share of each big subtree's time spent inside GX / OS / scheduler / indirect calls (first scene): update tree `body_80383200` 3% (its children 1-5%), draw traversal `body_8037BC0C` 77%, `body_80066A9C` 41%. So the update tree is pure guest compute: a candidate for parallel loops; the draw traversal is not.

Running the generator on a rejected function rewrites `runtime/hle/leaf_gen.cpp` and `tests/test_leaf_hle.cpp` to empty files (4292 and 1722 lines deleted in the working tree); `git checkout` restores them. Do not run it without that in mind.

## Result of option 2 (2026-10-31): ten more leaf functions native, about 0.5% of the guest thread

`tools/leaf_hle.py` now accepts conditional returns, `bdnz` loops, `ps_nmsub`, `stmw`/`lmw`; ten hot leaf functions were converted (`8005669C 80078A00 800FFD14 80138B1C 80235294 802647AC 80273674 80365C28 80374210 80394BC8`), each proven by the differential test (2000 randomised runs, memory, every register, lr/ctr/poll count equal).

Measured on the Deck with the stack sampler (first scene, owner's settings): the ten functions took 2.86% of the guest thread before (3.13% in the fight scenario) and 2.37% after, so the native versions save about 0.5 percentage points: the hybrid generator still runs every register-only instruction through the unchanged `ppc::op_*` and only the register file and the memory accesses get faster. Deck FPS, three alternating pairs at the owner's settings: old 68.7 / 69.2 / 66.5, new 66.0 / 68.2 / 67.3 (noise is 2-3%; nothing visible).

The larger candidates cannot be converted by this generator: `800A4434`, `8039A328`, `80137F78`, `803949B8`, `801371B4` and about half of the list are `body_` functions with extra entry points, because they call HLE or other guest functions and the scheduler may resume them after the call. Native versions of those would have to be resumable.

Not converted: `80369BCC` (the only difference was a NaN payload read back as an integer halfword: which payload a NaN-NaN operation keeps depends on the compiler's operand order) and `80369E68` (rare differences and "the native version faulted, the reference did not" in 3-4 of 2000 runs after the harness fixes; cause not found).
