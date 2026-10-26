# G20 — Dolphin reference captures and lighting/fog parity (2026-10-14)

Reference tool, not runtime code: Felk's Dolphin scripting fork (preview4) in `<dolphin dir>` (portable, Vulkan, 2x IR), driven by `work\control.py` (file commands: calibrated Wii pointer, buttons, savestates, frame capture via `framedrawn`) and `work\dctl.py`/`nav.py`. Savestate `work\train3.sav` = first train car after the tutorial; reference frame `work\ref_play.png`; FIFO log `work\train_play.dff` (3 frames, 2.9 MB FIFO + memory). `tools/spikes/dff_scan.py` parses .dff (header, frames, memory updates, CP/XF/BP state, indexed XF loads, display lists, vertex layout) and prints per-primitive lighting state and first-vertex view position/normal. Dolphin is only an oracle for state/pixels; no Dolphin code is used.

Findings:
- Lights, channel controls, TEV stages and konst selections of the train frame match our native packets.
- 28035 of 28512 normal-matrix XF writes per frame come from `GXLoadNrmMtxIndx3x3` (80288900, CP 0x28). It was not HLE'd, so its FIFO bytes were dropped and most meshes were lit with model-space normals (dark far door, dark right-hand seats). Native `hle_GXLoadNrmMtxIndx3x3` (live + recorded display lists, shared `indexed_normal` with the mesh path) fixes it; test_normal_matrix_hle RED→GREEN. Visual capture matches Dolphin's symmetric lighting.
- Fog: the game enables exponential fog (BP 0xEE..0xF2, colour #19141A). GXSetFog writes BP directly (only SDK side effect: bpSentNot=0); since 2026-10-15 a call-level native GXSetFog supplies the state (G21) instead of BP observation and applies hardware fog (A/B/C, perspective/ortho, fsel 2..7, 8-bit blend) after TEV/alpha test. Range adjust (BP 0xE8, GXSetFogRangeAdj) is not applied (TD-047).
- Character lighting in the Dolphin frame is also dark (colour channel 0 ≈ 31/255 average) and the zombie TEV setup matches ours; the brighter zombie in the reference stands next to the lamp above the camera. A same-moment comparison is still needed before claiming character parity.
- Mean luminance after both fixes: Dolphin 41.9, port 37.4 (different zombie positions).

## Audio gap audit (2026-10-14)

--audio-audit now writes bounded per-format/state/loop/SRC/mixer-control AX counters and actual pre-clipping PCM contribution to debug/audio-gaps-PID.txt. Diagnostics do not change playback. Native train route, PID24640: 26640 mixed blocks, 4361226 nonzero output samples; all active AX rows use supported ADPCM/PCM16 with main output. No dropped rows. This is not proof of audible parity.

Added compile-time-selected entry probes at verified RBUE08 WPADCanSendStreamData (802E3C10) and the stream-send routine (802E3C94: output report18, data pointer r4, length r5). Probe records requested bytes and existing connection/speaker flags without enabling hardware or replacing SDK results. Second full route PID50436: zero readiness queries and zero send attempts. Therefore that route does not exercise either entry; an earlier source-side gate is still possible. Missing button/card sounds remain unresolved, and no speaker-control/format coverage is claimed.

Evidence: build/autotest/1792008527786691200/run.log, build/autotest/1792008842110639400/run.log and build/game/debug/audio-gaps-{24640,50436}.txt. Both routes reached train and accepted native Reload/Grenade/Knife. Heavy frames and queue overruns remain TD-042; screenshot/profile runs are not clean performance measurements.

## Offline replay loader checkpoint (2026-10-14)

Own C++ DFF reader accepts v5/v6. Actual title/train captures are v5. Format field sizes follow the [upstream DFF structure definition](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/FifoPlayer/FifoDataFile.h); implementation is independent. File ranges and allocation counts are bounded before use. RAM updates apply in recorded FIFO order, with overlapping writes and previous-frame state preserved. Unrecorded reads produce an error instead of invented zero data. Tests and both real capture loads pass. This is the data layer only: command conversion, Vulkan rendering and same-frame image metrics remain pending. The library has no link into uc.exe.

User confirmed missing audio scenario: clicks on menu cards and menu buttons. Current AX/speaker audit has not explained those missing sounds.

## Offline Vulkan player checkpoint (2026-10-14)

`uc_fifo_replay capture.dff frame output.png [--validation]` now converts recorded geometry and material state, decodes textures with the existing native decoder, and renders through VulkanRenderer. It opens a visible window without activation/input capture. Title frame2 produced `build/fifo-title.png` (640x480); explicit Vulkan validation passed with third-party implicit layers disabled for that process. No pixel-parity claim: matching reference diff remains pending.

Texture uploads may be recorded at batch flush, after their first primitive. Initially unavailable RAM texture data is resolved at its first recorded update; later updates do not replace that owned snapshot. Existing resource bytes remain bound at the draw position. Register field interpretation was checked against the [Dolphin BP format declarations](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/BPMemory.h), and capture timing against its [recorder](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/FifoPlayer/FifoRecorder.cpp). No Dolphin implementation copied.

Train capture geometry traverses all3frames (53865draws/306276vertices), but full replay correctly refuses one unresolved initial EFB texture: address0x1305A760,160x120,RGBA8. The stream samples it before its first captured EFB copy and records no RAM source for it. Initial contents need separate evidence before comparing the train image. Unknown opcodes stop with frame/list/offset instead of guessing command length.

### RGB comparison CLI

Use `uc_fifo_replay capture.dff frame output.png --reference reference.png [--validation]`. Reference must match640x480; tool does not resize it. Writes `output-diff.png` with absolute per-channel RGB difference and reports mean/peak on0..255 plus changed-pixel count. Presentation alpha ignored. Output cannot overwrite reference. Title frame2 rerender vs previous native replay PNG gave mean0,peak0,0/307200changed: repeatability evidence only. Existing Dolphin train reference PNGs are1556x960 and not directly comparable.

## Automated same-frame oracle (2026-10-15)

scripts/dolphin_reference.py creates a fresh isolated profile, opens visible Dolphin without activation, captures six framedrawn RGB frames through the scripting API, pauses and closes only its own process. No desktop input. Output directory must be new. Native640x480,4:3,Vulkan; texture dumping enabled using Dolphin's documented Graphics.Settings DumpTextures key. Test callback validates exact byte counts, six-frame cutoff and completion metadata. Memory probing via scripting timed out before any frame; removed rather than retained as a working feature.

Title oracle: build/dolphin-title-reference-01/frame-2.png versus build/fifo-title-dolphin.png: mean absolute RGB1.447028/255,peak153,184165/307200changed. Reference frames0/3,1/4,2/5 match exactly. Frame2 is the closest matching index; biggest80x80 tile error4.442 at(240,160), around logo edges. This replaces the earlier native-vs-native repeatability-only measurement.

Train oracle: build/dolphin-train-reference-03/frame-2.png; earlier run02 has identical frame2. DumpTextures produced exactly one160x120RGBA8 RAM texture, tex1_160x120_a6c477a4cdc8b783_6.png, allRGBA bytes zero. Controlled diagnostic copy build/train-oracle-seeded.dff adds a position0/frame0 update of76800zero bytes at0x1305A760, preserving every original command/update. Sidecar .json records source/outputSHA256 and provenance. Original DFF unchanged. This seeds the observed fresh-oracle baseline, not original live-game framebuffer history. Unseeded replay still rejects missing memory; no runtime fallback added.

First seeded native comparison mean40.833257 revealed an offline-importer error: LightUniform.color expects byte-unit energy0..255, but gx_state normalized it0..1. Native HLE already uses0..255. Regression test failed before fix and passes after it. Corrected build/fifo-train-dolphin-lit.png vs reference frame2: mean3.662187/255,peak255,258917/307200changed; explicit Vulkan validation clean with implicit overlays disabled only for replay process. Neighbouring reference-frame means15.307265,10.835924 confirm frame2 alignment. Train loop frames are not byte-identical: frame5 mean3.663926, so persistent EFB history still matters.

Largest80x80 errors (mean,x,y): (8.3109,320,80) lamp; (6.0962,480,160) right wall; (6.0482,80,160) left wall/head; (5.8284,160,160) character/wall. Glow, filtering and edge differences remain; no full parity claim. FullCTest88/88 passed2.82s. Game audio/menu sounds and live-game rendering unchanged by this offline fix.

### STQ diagnostic step (2026-10-15)

Controlled Dolphin DisableCopyFilter=True run produced identical frame2 to baseline (build/dolphin-train-no-filter-01); filter toggle does not explain this reference mismatch. Active TEV UV inspection found404draws in frame0 using position/normal STQ; other slots also use post matrices. XF field meanings verified against upstream XFMemory.h declarations, no implementation copied.

Offline GxStream now bakes regular UV/position/normal/binormal sources, indexed texture matrices, AB11/ABC1, ST/STQ, dual post matrices and optional normalization into owned vertex STQ. GPU coordinate Q survives interpolation and division occurs in fragment shader. Native game HLE still produces existing UV path with Q=1: runtime texgen expansion remains TD-036, no live-game visual improvement claimed. CPU bake is offline-only; vertex upload grows144→176bytes to carry8Q values. Unsupported nonregular texgen fails clearly; original missing-memory strictness unchanged.

Tests: position STQ+post transform; normal source+normalizing post; GPU varying-Q fixture distinguishes fragment division from predivided vertex coordinates. Renderer smoke --validation passes (including existing HUD/scales/indirect scenarios). Full build.bat --no-install passes88/88 in6.94s. Train same-frame comparison mean3.653983/255,peak255,258904changed vs prior3.662187: small improvement, lamp error remains. Artifacts build/fifo-train-stq.png and -diff.png. No claims about missing menu sounds.

STQ verification/install: cavecrew-reviewer(gpt-6-luna) reported no Important/Critical issues. Visible audible native route PID56200 reached train/gameplay, screenshot frame-1792012716074937-56200-17336.png shows rendered scene/HUD/explosion. Route script exit1: expected two Knife accepts, observed one; Reload/Grenade/Knife all individually accepted, no fatal/guest trap in inspected log. This is not a passing full control route or a clean FPS measurement. Audio menu PBs at0x80609CE0/0x80609E20/0x8060A0A0 generate nonzeroPCM; character/weapon-selection clicks still require source-side comparison. dist copied with robocopy /E, staged/installed uc.exe/replay/shaders byte equality verified. No commit.


## Menu click source audit — 2026-10-15

Bounded opt-in --audio-audit probes added at named request80068DD8 and verified lookup/gate/start/group-loading entries. Snapshots preserve entry arguments and return r3, no guest mutation. Requests cap128, return records512; disabled audit retains nothing. test_audio_gap_audit checks snapshot ownership, invalid string pointer, caps, disabled mode and scope return values.

Visible internal-input runs build/autotest/1792013477981565500,1792013649995807300,1792013763566214000,1792013875664859100 reproduced: SY0101(id573),SY0105(id57B),SY0102/4 names resolve;8004C084 gate returns1; type3;803ABAB8 returns4.803A9C74 returns4 when803A9230 cannot resolve file1FD (509). Working SY0300/SY0400 uses file1F7 and returns0. biosound.brsar INFO confirms file509 belongs to group12; group data exists (0x4C9DC0,0x9C0bytes; wave0x4CA780,0x83A0bytes).

First group12 call803A9DEC from80049DFC returns0 before803A6748/803A7E5C are reached: player+0x10 archive pointer is still null. Only afterwards manager update8004A3CC creates player803A87FC and sets manager+0x1FA4=2. Later group11/54..63/9/10 succeed. Worker80102CE4 marks the group request complete unconditionally at80102D94..DA4 despite load failure, so group12 is never retried. A visible15s run without --skip-intro reproduces the early failed group12 request: intro skipping excluded. Next: establish native dependency for group loading vs player initialization; inspect queue ordering/scheduler boundary before choosing fix. No mixer gain or synthetic click workaround.

Limit: missing sounds remain unfixed; only diagnostic build/game/uc.exe updated, dist/installed game unchanged. Route scripts later hit existing pause/acknowledgement checks or80s lifetime; not counted as full route passes. No commit.


## Menu click readiness fix — 2026-10-15

Native startup dependency before translated80049D64 now yields until manager player exists and initialization state2. The loader itself, archive decoding and AX mixer remain unchanged. Known-manager/disabled bypass;10s diagnostic failure prevents endless initialization wait. TD-048 tracks profile-specific polling and unproven original scheduler parity. New scheduler test makes urgent worker arrive before main initialization, verifies it waits, later resumes and preserves arguments; ready/disabled/unrelated calls bypass.

Visible audible internal route PID38988,build/autotest/1792014190108103400/run.log: group12 load now returns1. SY0101(id573) and SY0105(id57B) start returns0 instead of4. SY0101 generates ADPCM main-route voice peak5377 (frame3265..3291), SY0105 peak8391 (frame4349..4441), repeated successfully through menu selections. This verifies restored real PCM contribution; no fake sound or gain change. Full build.bat --no-install exit0,89/89 tests5.55s.

Verification/install follow-up: strengthened readiness test proves archive-pointer publication alone cannot release waiting worker before state2; targeted test passes0.17s. cavecrew-reviewer(gpt-6-luna):0 high/medium,1low (timeout/cleanup regression test deferred, tracked TD-048). Route PID38988 ended normally at80s (all workers joined, no fatal); script exit1 expected next acknowledgement after window lifetime, so full route not claimed. Installed dist with robocopy /E; uc.exe byte-identical SHA2567cba31a937448857ede304a40ae765326d97fcb612541111953e7d185ce4155c. No commit.
