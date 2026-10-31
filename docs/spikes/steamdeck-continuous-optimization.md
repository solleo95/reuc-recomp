# Continuous Steam Deck and Windows optimisation

Current owner instructions, 2026-10-31: continue autonomously on Steam Deck only. Windows builds, runs and validation are deferred until the owner requests that phase. Record retained improvements, Deck evidence, correctness checks and saved binaries in [the Windows validation backlog](steamdeck-windows-validation-backlog.md); keep unconfirmed candidates separate. Later, check applicable changes individually on Windows and exclude confirmed regressions. This supersedes immediate both-platform checks. A finished experiment or review does not end the ongoing search. Preserve quality, render scale, clocks and prior uncommitted work; no commits or pushes unless requested.

Loop: profile -> one stated hypothesis -> exactness check -> one-variable experiment -> deterministic benchmark -> short live confirmation -> keep or discard -> next hypothesis. Record rejected hypotheses so they are not repeated without new evidence. Review substantial completed changes; review is a checkpoint, not the completion condition of this ongoing request.

Owner clarification: Max FPS in the same scene is an important success metric. Report Max alongside mean, lowest steady second, p99 and worst frame; a repeatable Max improvement is a valid benefit even when mean barely moves. Never reject a candidate solely on mean FPS. Preserve compared binaries while checking conflicting metrics. Match settings, scene/time and speed before attributing an observed peak to a change; distinguish owner effects from default-effects runs and startup/transition spikes.

## Completed

- EFB-copy final-mip transition: one fewer Vulkan dependency command per copy. Exact GPU smokes, Linux/Windows builds, CTest 231/231, both platforms benchmarked. No resolved FPS gain; exact small reduction kept. Results: `P1-efb-copies.md`.

## Current investigations

1. Vulkan depth barriers with owner effects enabled: determine whether the tracked depth layout already proves the previous use was depth attachment, making the extra effect guards on the narrow wait redundant. Check every depth-image consumer and readback before editing.
2. Guest GX TextureBank rebuild: trace all matrix writes and restore/replay invalidation before proposing an O(1) cache hit. Prior transform-dirty no-gain experiment is not repeated blindly.
3. Audio `8039A328`: check all callers and resumability/interrupt semantics before replacing a pure part; full AX sync remains a broader candidate behind that proof.

## Measurement constraints

Use the existing uctest harness, isolated saves, muted audio and 1280x720 window. Before each Deck game run, check no user game is active. Keep Deck test files temporary. Owner update: all Deck test runs are uncapped through valid test-server plus explicit CLI FPS zero, including owner live comparisons; keep per-pixel lighting, ultra motion blur and bloom150. Ordinary Linux launches retain maximum90FPS. Historical owner comparisons below used90 and must be labelled separately from new uncapped runs. Prefer short scenes and the recorded opening demo; heavy-fight claims need their own matched-work evidence. Measure both owner effects and defaults, and use Windows counterparts for shared changes. Repeat only when noise prevents the keep/drop decision.

Keep measurement modes explicit. Deterministic demos advance virtual game time and replay identical work; the scene can appear in slow motion when that work takes longer in wall time. Judge them by wall time/cycles for matching retraces/draws. Their displayed FPS is not the player's ordinary-speed Max. Live first-scene comparisons run at speed1 and supply actual Max/mean/pacing; report those separately. Instrumented diagnostic runs establish counts/correctness, never FPS gains.

## Depth-attachment wait result

Removed unrelated capability/effect conditions from the narrow depth wait. A tracked depth-attachment layout means depth tests were the previous users; all effect/upscale readers move the image to GENERAL, whose broad dependency remains. Same Vulkan implementation serves Linux and Windows; D3D has no corresponding redundant stage-mask wait.

Both platforms build. GPU pixel/copy/resize/upscale, lighting golden/blur, GTAO/G-buffer/contact/shadows/RT-shadows tests passed. Full CTest passed 231/231. Independent item review: no issues.

Owner live comparisons (first two log records excluded, eight steady seconds):

| Order / binary | Mean FPS | Max FPS | Lowest second | Mean per-second p99 / worst frame, ms |
|---|---:|---:|---:|---:|
| first / before | 68.32 | 75.66 | 58.93 | 17.82 / 22.93 |
| second / after | 69.80 | 71.92 | 60.65 | 16.60 / 21.04 |
| first / after | 70.91 | 73.97 | 66.88 | 15.60 / 17.91 |
| second / before | 69.12 | 73.83 | 59.90 | 16.96 / 21.71 |

Defaults: before mean 78.21, Max 83.99, lowest 68.99, p99 15.04/worst17.56ms; after mean78.95, Max83.00, lowest74.88, p9914.86/worst16.50ms. In this series, peaks80-83 belong to defaults for both binaries; they do not establish a Max gain against the owner-effects runs around70. Preserve both binaries while checking conflicting metrics. The repeated owner pairs support a pacing/steady-mean benefit, with no resolved Max improvement.

Deterministic opening owner: Deck10.027->9.997s, same443158draws; render-scale2 probe19.589->19.632s, samework. Windows owner3paired before2.999s/after3.013s mean (+0.5%,95%CI[-3.7,+4.7], inconclusive); defaults3.056->2.944s (-3.7%,onepair inconclusive). No confirmed Windows regression. Keep common Vulkan path for both platforms. Saved pre-depth baselines: build/uc_depth_before and .exe; verified depth snapshot for later CPU A/B: build/uc_unit_before and .exe.

## CPU candidates after fresh profiling

Fresh9s first-scene profile `build/autotest/1793422592143891600/cpu-samples.txt`, matched ELF `build/uc_depth_before`, analysis `build/texbank-profile.txt`: 9833samples, guest6639. TextureBank is absent from the top120 out-of-line guest symbols; defer invalidation machinery until a scene shows material cost. Leaf80394374 remains2.3% self, so investigate its unit-period subtraction loop and sibling803942AC instead. Batch only exact-float inputs in[2,2048), dynamic period==1 and iterations strictly before the next safe point. Keep original loop for every other input and preserve final comparison/interpolation. No quality or lookup-table change.

## Single-page write-watch result

Added an explicit scalar stamp when a write range touches one page. Existing rollback tracking and generic multi-page clipping remain. Optimized code confirms the four-byte hot marks avoid masked AVX loops. Range-edge tests, both builds, exact before/after guest traces over1634retraces and full CTest232/232 pass. Item review: no introduced issues; a suggested overflow-semantics change was pre-existing and outside this semantics-preserving experiment.

Windows opening owner3paired:3.016->2.928s (-2.9%,95%CI[-5.5,-0.2], FASTER), Max1s198.0->203.0FPS. Deck deterministic9.335->9.357s (+0.2%,neutral), same443158draws; guest cycles6406->6299M (-1.7%). As expected, this serialized GPU-bound replay does not resolve the live CPU benefit.

Deck owner live, steady8seconds after two transition records:

| Order / binary | Mean FPS | Max1s FPS | Lowest second | Mean per-second p99 / worst frame, ms |
|---|---:|---:|---:|---:|
| first / before |69.490|73.885|61.859|16.568 /20.31|
| second / after |70.789|73.723|63.610|16.415 /20.26|
| first / after |72.008|75.740|67.898|15.446 /17.14|
| second / before |69.443|74.970|62.773|16.999 /20.36|

Full10-record maxima, including the transition: first pair73.9->74.9; reversed pair before75.0/after75.7. Keep raw and steady metrics separate so removing transition noise does not silently erase a possible peak benefit. Native Leaf80394374 self falls2.7%->1.8% in the matched9s profiles (7195/6702 guest samples). Mean/pacing improve in both orders; steady Max is essentially preserved in one pair and rises in the other. Saved binaries build/uc_watch_before[.exe] and build/uc_watch_after[.exe]. These include the still-evaluated unit-period candidate; a temporary diagnostic will count its actual use before the next deployment.

## Unit-period batching — deferred after runtime proof

The exact arithmetic helper and its differential tests passed, but real inputs invalidate its performance premise in this scene. Diagnostic1793426240523425700:3905985calls, zero period==1 and zero fast-path hits. Follow-up1793426401460514200: actual period65536,3157444calls, zero arguments reaching even two periods (four/eight also zero). Earlier Max/mean differences are retained in the ledger; no repeatable Max benefit was established. Do not extend batching speculatively to a different period when there is no repeated subtraction to remove.

Archived sources under build/unit-experiment-sources and preserved binaries. Removed only this candidate and its dedicated tests/generator additions; prior native leaf work stays. All diagnostic counters removed. Validate the retained write-watch change alone against the saved pre-unit binaries before deploying it. Revisit batching only with a profile that demonstrates real repeated-subtraction work in another scene.

Clean isolation after removal: both builds pass; leaf tools9/9 and runtime5/5 pass. Deck deterministic pre-unit baseline9.363s/clean9.368s, equal443158draws, cycles6592->6367M. Windows3paired baseline3.034s/clean3.050s (+0.5%,95%CI[-4.7,+5.7], INCONCLUSIVE), Max197.0->199.9FPS. This clean Windows comparison supersedes the earlier resolved2.9% claim as the final isolated result: no confirmed regression, no resolved mean gain. Live clean Deck confirmation is still running. Do not attribute the earlier incidental code-layout/noise differences to a helper whose fast path never executed.
