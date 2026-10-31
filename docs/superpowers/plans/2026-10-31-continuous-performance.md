# Continuous performance implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Continue exact performance optimisation on Steam Deck only. Windows work is deferred; record retained improvements in `docs/spikes/steamdeck-windows-validation-backlog.md` for later individual Windows checks. Continue until the owner stops the ongoing goal.

**Architecture:** One measured hypothesis at a time, preserving baseline binaries and correctness evidence. Current builds, runs and checks target Deck only; earlier Windows results remain historical. Later Windows validation excludes confirmed regressions. Select the next hypothesis from measurements.

**Tech Stack:** C++20, Vulkan/D3D renderers, PPC runtime, existing Python 3.13 uctest.

**Spec:** `docs/spikes/steamdeck-continuous-optimization.md` and the ongoing user goal.

## Global Constraints

- Keep quality, render scale and clocks. No commits or pushes. Preserve all existing uncommitted work.
- Native execution in the current checkout, authorised by the owner without further questions.
- Test via existing harness only, isolated saves, small/windowed GPU smokes and 1280x720 game windows. Check Deck idle before every run and clean experiment files.
- Prove output correctness before benchmarking. Compare deterministic matched work, then short live scenes on Deck only. Record all retained improvements for the later Windows phase; no new Windows builds or checks now.

## Review Focus

- A tracked GENERAL depth layout after compute/upscaling keeps its broad dependency.
- Repeated attachment-only depth work waits only for earlier depth tests, regardless of unrelated effects/capabilities.
- Scene restore, camera cuts, resize and optional renderer features retain exact output.
- CPU matrix caching must cover all live, import, replay, checkpoint and reset writes.
- Audio helpers must preserve scratch registers, aliasing, IRQ nesting and resume points.

## Task 1: Narrow attachment-only depth waits with effects available

Files: `renderer/vulkan_renderer.cpp`, `tests/renderer_smoke.cpp`; results in `docs/spikes/steamdeck-continuous-optimization.md`.

- [x] Extend the existing real-driver barrier wrapper to count DEPTH_ATTACHMENT-to-DEPTH_ATTACHMENT barriers and broad source waits. Existing repeated-frame fixture must produce at least one such barrier and zero broad waits. Watch the old implementation fail.
- [x] Use `initialized && depth_before==VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL` as the narrow-wait condition. The tracked GENERAL layout already identifies effects/upscaler readers. Keep their full wait, initial NONE and all other image barriers unchanged.
- [x] Build and run the pixel/copy smoke plus effect and upscale smokes; exercise depth-reading effects too. Preserve and record pre-existing external/validation failures separately.
- [x] Cross-build and compare saved before/after binaries on Deck and Windows with owner effects and defaults. Keep only correct changes; reject Windows regressions there. Record CPU/GPU/FPS/pacing evidence.
- [x] Review this completed iteration, deploy only verified variants, then continue Task 2 instead of ending the goal. Pre-depth binaries are retained locally; remote variants are reused only while the adjacent CPU comparison is active and cleaned at its checkpoint.

## Task 2: GX TextureBank cache investigation

- [x] Locate the bank construction and all matrix write/restore paths; no existing usable version. Distinct from the prior no-gain transform-dirty experiment.
- [x] Fresh 9-second Deck flat profile: texture_bank is absent from the top 120 guest symbols. Defer the versioned cache until another scene demonstrates material cost; avoid adding invalidation complexity speculatively.

## Task 3: Audio native-block investigation

Outcome: implemented/proved/tested, then removed after real-input diagnostics (3905985calls, zero period==1/hits; actual period65536, another3157444calls with no two-period arguments). Experiment sources/binaries archived. This item is complete as a rejected measured hypothesis; it does not end the ongoing goal.

- [x] Locate all callers and IRQ/resume boundaries of 8039A328; only A364-A54C is a pure uninterrupted block. Whole-function replacement needs resumability.
- [x] Full audio-body replacement needs IRQ/resume handling. Chosen smaller measured leaf: 80394374 (2.3% self) and its sibling 803942AC share a positive unit-period float subtraction loop.
- [x] Add `uc::leaf::reduce_unit_period(double period,double& value,double& paired,uint32_t& polls)` in `runtime/hle/leaf_math.h`. It accepts only period 1, exact-float values in [2,2048), and iterations strictly before the next 2048-poll safe point; otherwise leaves every argument unchanged. Test exact output/poll counts against literals and 10000 independent repeated-subtraction cases (`tests/test_leaf_math.cpp`, CTest registration); observe RED first.
- [x] Recognise only the exact fsubs/fcmpo/cror/backedge shape in `tools/leaf_hybrid.py`, emit the helper at the initial forward jump, and keep the original loop and final comparison as fallback. Add the include in `tools/leaf_hle.py` and `runtime/hle/leaf_gen.cpp`, and regenerate only the two affected entry jumps without touching prior generated functions. Test recogniser acceptance/rejection and full leaf differential output, registers and polls.
- [x] Cross-build, demo-check exact guest state, benchmark both platforms and confirm live Max/mean/pacing. Keep or discard independently for Windows if it regresses. Continue subsequent hypotheses after the iteration.

Further tasks are selected from the resulting profiles. Completed iterations and rejected candidates remain recorded; this plan's queue being briefly empty does not finish the ongoing goal.

## Task 4: Single-page GX write marks

Outcome: kept and deployed cleanly after Task3 removal, both platforms tested, exact replay and full suite pass. Clean Windows mean remains within noise, Max197->199.9; clean Deck steady mean69.305->70.715, Max74.949->77.999. No confirmed Windows regression. Baselines and results in the ledger.

Files: `runtime/gx_write_watch.h`, `tests/test_ppc_float.cpp`.

- [x] Save the current unit-period candidate separately as the next binary baseline; keep that candidate's decision pending the reversed Deck pair.
- [x] Add explicit empty, single-page, crossing, bulk, alias, end-of-address-space and overflow range cases, with rollback tracking both off and on. Check the existing semantics before the performance-only change.
- [x] Give single-page ranges a scalar page stamp before the generic multi-page loop. Preserve rollback, clipping and overflow semantics. Inspect optimized code: the old four-byte mark emits a masked vector loop at sampled hot instructions.
- [x] Build both platforms, run runtime/rollback correctness checks and exact guest-state replay, compare deterministic work and live Max/mean/pacing. Keep only measured correct improvements; test Windows independently.

## Task 5: Fixed-width GX FIFO recording stores

Files: `runtime/hle/gx.cpp`, `tests/test_gx_hle.cpp`.

- [x] Save a verified binary baseline after resolving the unit-period candidate. Preserve all producer, capacity, overflow and primitive checks.
- [x] Strengthen the existing recording fixture with nonzero mixed-width payloads, truncated high bits and an unaligned16-bit write. Run against the current implementation before editing the performance path.
- [x] Replace only the recording fast-path byte loop with fixed1/2/4-byte stores, using the existing byteswap+memcpy pattern for unaligned host buffers. Leave the general fallback intact. Inspect optimized code to confirm the large dynamic AVX loop disappears.
- [x] Run GX recording tests, both builds and exact replay, benchmark Windows and Deck, including live Max/mean/pacing; retain only a correct improvement and continue the queue. Kept for measured CPU-work reduction; live Max/mean gain remains unresolved and the sampled p99 regression did not reproduce without the sampler.

## Task 6: Refresh attribution after the GX changes

- [x] Resolve the new before/after9s Deck profiles; use actual CPU/sample changes to select the next candidate.
- [x] If TextureBank still hides behind library copy/compare work, count eligibility and measure that path temporarily before adding any versioning. A low eligibility/cost result defers it again.595372calls,zeroeligible:deferred.
- [x] Reassess audio delta hoisting/int32 envelope multiplication against the remaining measured mixer cost. Preserve running/end checks, exact PCM and PB state, all formats and subsequent64bit gain multiplication. Delta hoisting chosen; int32 arithmetic stays separate/deferred.
- [x] Continue one measured production change at a time; no speculative caches or broad audio replacement.

## Task 7: Cache invariant audio mixer deltas

Files: `runtime/ax_audio.cpp`, `tests/test_ax_audio.cpp`. Begin with tests while Task6 attributes TextureBank cost; choose production priority after those measurements.

- [x] Add an independent finite PCM16 reference test for envelope/left/right sweeps, signed sample extremes, clamping, disabled channels, stopping mid-block and looping. Compare the192mix values and final PB fields; validate existing implementation first.
- [x] Hoist only the three invariant signed16 deltas at offsets0x6C,0x16,0x1A outside the96sample loop. next_sample writes running/position/ADPCM state, never these deltas. Keep running/end checks and64bit arithmetic exactly as before; int32 multiplication is a separate future hypothesis.
- [x] Run audio and rollback checks, both builds, exact guest replay and matched Windows/Deck measurements including Max/pacing. Keep or discard based on correctness and hardware evidence, then continue. Both hoists deferred: three-delta variant trades lower Max for mean/min, envelope-only lacks repeated Max gain and lowers mean. Original mixer restored on both hosts; archives and160-case reference retained.

## Next evidence-backed GX queue

### Material snapshot copies (next production candidate after audio)

File: `runtime/hle/gx.cpp`; existing GX/mesh/texture-matrix tests exercise it.

- [x] Preserve the resolved audio/FIFO baseline before editing.
- [x] Return `const MaterialSnapshot&`. Build the result directly in `material_cache.result` while `valid` is false: assign its material to `gx::TevMaterial{}` and uniforms to `color_uniforms`, then populate existing fields. Remove the final self-copy, set cache metadata/valid only after completion, and return the canonical result. This avoids adding a miss-path copy when hits are only13%.
- [x] Change only the two read-only GXBegin structured bindings to `const auto&`; leave mesh's mutable binding and import's immediate uniforms copy intact. Preserve cached/fresh value copies in cache_verify recursion. Document the borrowed result lifetime through the next snapshot call.
- [x] Run existing GX/mesh/texture-matrix ownership cases with cache verification on/off, both builds and exact replay. No cache mismatches and exact PCM; both ordinary Deck pairs regressed, so the candidate was archived and reverted. Both snapshot variants retained PGO.

### Material miss attribution (after rejected copy candidate)

The copy candidate is correct but slower in both ordinary Deck pairs and has no resolved fixed-work gain. Archive it and restore the FIFO source before the next hypothesis.

- [x] Count misses with unchanged full SDK raw bytes, then compare their recomputed material/uniform output against the previous snapshot. About half the misses recomputed equal output; caller attribution is recorded in the ledger.
- [x] Build temporary diagnostic ELFs, restore production source byte-for-byte before running, use isolated deterministic DeckRun and verify full PCM against the baseline. No performance inference from instrumented runs.
- [x] Select the small matrix-index mask experiment before broader dependencies; SDK dependency ranges remain the next candidate.

### Matrix-index material dependencies (next minimal candidate)

Files: `runtime/hle/gx.cpp`, `tests/test_texture_matrix_hle.cpp`.

Diagnostic2 attributes37601equal-output pureepochmisses and44237equal-output rawchangedmisses to set_matrix_index. Its word0 also contains the position selector; material_snapshot reads only24texture-selector bits. Try this before general matrix dependency machinery.

- [x] Save the verified FIFO baseline, extend selector tests and pass them before the edit. Exercise reg1 texture selector changes with SDK bytes unchanged, low position/high reserved bits, and recording an identical index followed by replay against a different current position.
- [x] In set_matrix_index, bump material_epoch only for unknown previous value or changes under reg0mask0x3FFFFFC0/reg1mask0x00FFFFFF. Always store the full word, record its command and update transforms.current as before. Keep all other invalidation paths.
- [x] Run GX/texture/mesh ownership and cache-verification tests, both builds and exact real-game checks. Exact1634-retrace guest state and full9072-block PCM; item review clean. No repeatable live Max/mean gain, fixed work neutral/slower: archive and revert production mask, retain ownership tests. Results in ledger.

### Fresh isolated PGO checkpoint

- [x] Audit profile attachment rather than infer it from warning totals. Three-delta mixer loses its profile on both platforms; envelope-only and borrowed snapshot retain theirs. Current accepted FIFO recording function also lacks its old profile. Preserve every old binary and canonical project profile.
- [x] Build instrumented accepted source with existing generation trees; train the exact short opening replay into build-local directories. Verify executed binary and profile counters; failed startup/live scenarios excluded from matching training.
- [x] Merge isolated augmented profiles, preserve pure-fresh controls and canonical profiles. Build and check exact guest/PCM output, benchmark both platforms including Max/mean/pacing. No repeated Deck benefit from augmentation alone; keep archived for fair audio comparison. Both normal build-cache paths restored to canonical profiles.
- [x] Revisit three-delta audio with matching supplemented profiles: both platforms apply candidate branch data,870912trainingcalls each. Exact output and Windows checks pass. Correctly bounded Deck1x Max rises against fresh-profile control in both orders, but does not repeat against installed canonical-profile build. Preserve candidate/profiles, restore original mixer; pure-fresh-only profile comparison remains a later control. No audio deployment.

### Test-only uncapped Steam Deck runs — owner steering

- [x] Find both restrictions: explicit90 in deck_perf owner preset and Linux CLI zero clamping to90. Preserve normal/config maximum90 and game pacing.
- [x] Add test-only zero exception to the shared cap helper and defer CLI normalization until test-server arguments are known. Keep config clamp unchanged. Check normal values, both CLI orders and configured90 override in the shared cap test on Windows and native Deck. RED observed before implementation.
- [x] Owner test preset now explicitly uses0. Real isolated Deck run logged fps_limit0 and menu306.735FPS, compared with historical capped maximum89.993. All231Windows tests pass; Linux native cap/parser test passes. Keep historical capped measurements separate from new uncapped comparisons.
- [x] Exact1634-retrace Windows replay and full9072-block Deck PCM/cacheverify pass; full231/231 suite passes. Deploy verified test-only exception on Deck and Windows; hashes in ledger. Clean next-run and temporary variants. Resume fresh-profile audio comparison with the exception present in BOTH baseline and candidate; keep historical capped measurements separate.

### Full speed-1 measurement windows

- [x] Diagnose raw Max180-200 after test uncapping: old last-N log selection included a mixed x8-to-x1 second. Preserve old logs and do not interpret these transition peaks as gameplay gains.
- [x] Record perf retrace bounds immediately around measurement, after capture. Filter complete >=1000ms profile blocks fully inside that range; keep pacing/GPU lines from the same blocks. Keep the earliest valid second, rather than dropping a fixed two seconds. Reject missing bounds explicitly.
- [x] Regression tests preserve an early valid Max83 while rejecting transition250 and later500, align pacing/GPU, reject partial windows, and prove bounds bracket measurement after capture. RED before implementation; both harness test suites pass.
- [x] Actual Deck output selects nine complete1x seconds and preserves earliest real Max. Item review No issues. Complete audio control and stock BAAB, record each separately; no inference from mixed transition peaks. This is a measurement correction, not a gameplay change.

### Validated SDK span reads

Files: `runtime/hle/gx.cpp`, `tests/test_texture_matrix_hle.cpp`.

- [x] Inspect original material_snapshot IR and Memory::read alias/bounds semantics. Full0x600 SDK validation already precedes all field reads;33inline field checks and34staticfallbacksites remain in optimized IR. Save installed canonical-profile/test-cap baseline build/uc_sdk_before[.exe]; original mixer restored before starting.
- [x] Extend packet fixture with copied SDK in Mem2 aligned/unaligned, nonzero material/pixel/color fields and old packet ownership; reject truncated SDK and recover. Original implementation passed first. Negative case is rejected by preceding GXBegin dirty-word read, so does not isolate material_snapshot guard; that full guard stays unchanged by source proof.
- [x] In material_snapshot only, add local big-endian32bit load from validated block with memcpy+byteswap. Replace only read32(sdk+offset) field reads. Keep SDK-pointer read, full validation, selectors, epoch/cache logic and all output arithmetic unchanged. Three GX tests normal/cacheverify pass.
- [ ] Run GX/mesh/texture tests normally and with cache verification, both builds and exact real-game checks. Train matching profile if the function CFG changes; use isolated profile files. Compare against installed baseline on both platforms and complete1x Max/mean/pacing before retaining anything.

- Avoid eager `read32` fallback arguments in `texture_selectors`: all three helper callers already validate the full0x600SDK block. Existing texture-matrix tests cover override/reset/record/replay; keep fallback validation. Two conditional reads, one file, after the current audio decision.
- Inspect repeated SDK bounds/alias checks in material_snapshot after the audio decision: it already validates memory.read(sdk,0x600), so fixed-offset big-endian reads could use that validated span directly. Preserve full validation and host matrix/color invalidation. Inspect generated code first; train a matching profile if the CFG changes. This is a separate hypothesis from invalidation masks and borrowed snapshot copies.
- Borrowed/in-place snapshot copying was already tested and rejected in Task8. Revisit only with new attribution evidence; it is not an untried queue item.
- At a verified checkpoint compare the cumulative build against the preserved early baseline with identical settings, recording Max as well as mean/pacing. This does not end the ongoing goal.
