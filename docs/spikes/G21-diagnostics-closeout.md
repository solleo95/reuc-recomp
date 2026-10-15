# G21 — Diagnostics and FIFO replay close-out (2026-10-15)

Plan: `docs/superpowers/plans/2026-10-14-diagnostics-and-fifo-replay.md`. Evidence and measurements: `G20-dolphin-reference.md`. Remaining gaps: `docs/tech-debt.md` (TD-036, TD-044, TD-047, TD-048, DFF-01..04).

## What exists

| Tool | Use | Output |
|---|---|---|
| Crash reporter | always on in `uc.exe` | `<exe>\debug\crash-*.txt`: reason, host frames mapped to translated guest functions (`data\uc.map` for runtime frames), runtime stats |
| `uc.exe --gx-gaps` | GX commands no HLE owns, attributed to the writing guest/SDK function | `debug\gx-gaps-*.txt` at exit |
| `uc.exe --audio-audit` | AX voices by format/state/loop/SRC/mixer, PCM contribution, WPAD speaker entry probes, menu sound request/group-load probes | `debug\audio-gaps-<pid>.txt` |
| `tools\uc_fifo_replay.exe capture.dff frame out.png [--validation] [--reference ref.png]` | renders a Dolphin FIFO log through `VulkanRenderer`; offline only, not linked into `uc.exe` | PNG, `out-diff.png`, mean/peak/changed pixels |
| `scripts/dolphin_reference.py capture --output new_dir` | same-frame 640x480 Dolphin reference frames from a fresh isolated profile | `frame-N.png` |

## Owner-reported gaps

- Crashes: reports written and attributed (verified on a forced startup failure).
- Static title background: indirect texturing (BP 0x06..0x27, IREF) observed natively and applied in `tev.slang`; title animates. Offline title vs Dolphin frame2: mean 1.447/255.
- Missing menu card/button clicks: group 12 of `biosound.brsar` was requested before the sound player existed and never retried. Loader now waits for player readiness (TD-048). Owner confirmed clicks audible in game, 2026-10-15.

## Open

- Train same-frame parity: mean 3.654/255, worst tiles at the lamp/glow and wall edges (filtering, copy filter excluded, EFB history DFF-01).
- Live-game texgen still UV-only with Q=1 (TD-036); offline replay bakes full regular texgen.
- `scripts/train_route.py` has not completed a full pass after these changes (second Knife accept, 80 s window lifetime); individual native actions are accepted.
- WPAD speaker stream path is not exercised by the tested routes; no speaker format coverage claimed.

## Renderer plan Task 1 gate — PARTIAL (2026-10-15)

Run: `scripts/train_route.py --gx-gaps` (new passthrough), report `dist/debug/gx-gaps-6476.txt`, log `build/autotest/1792015770927022000/run.log`; reached train, ended at the 80 s lifetime (known route acknowledgement limit). 1 280 066 draws, 25.8 MB of FIFO bytes outside primitives, 0 unmapped MMIO.

The report lists who writes each command, not what is lost: native draws read most state from the SDK shadow (`material_snapshot`/`snapshot` in `runtime/hle/gx.cpp`) or observe BP (fog, indirect).

Owned: VCD/VAT, matrix index, viewport, scissor, cull, TEV colour/alpha/order/konst/swap/stage count, Z mode (shadow 0x228), blend/cmode0 (0x220), dst alpha (0x224), channel controls/material/ambient, UV texgens and texture matrices, fog (BP 0xEE..0xF2), indirect (BP 0x06..0x27).

Unowned classes (writer → registers; count per run):
- `GXSetZTexture` → BP F4/F5 (57 518 in lists): Z textures not implemented; whether the op is ever enabled is unverified.
- `GXSetCoPlanar` → BP FE + GEN_MODE bit 19 (304 275 live): coplanar decals ignored.
- `GXSetZCompLoc`/`GXSetPixelFmt` → BP 43 (≈185 000): early/late Z and EFB pixel format (alpha availability) ignored.
- `GXSetPointSize`/line width → BP 22 (71 860): host lines/points are 1 px, unscaled by `--render-scale`.
- `GXSetScissorBoxOffset` → BP 59 (9 904 live): offset assumed zero, value unverified.
- `GXSetClipMode` → XF 1005: clip disable ignored.
- `__SetSURegs` and NW4R direct writers (80373DB0, 80375B58, 80376BB0) → BP 30..3F: hardware texcoord scale/bias flags unread; native normalises by texture size.
- Non-UV texgens (XF 1040/1050 via `__GXSetDirtyState`) → TD-036; fog range BP E8 → TD-047; copy filter BP 01..04/53/54 → TD-034.

Value probe over all 3 frames of the Dolphin train capture (`build/probe/bp_probe.py tools/spikes train_play.dff`, 53 865 primitives: 51 141 strips, 2 034 triangles, 549 quads, 141 fans, no lines/points):
- Default or disabled everywhere, no work needed: Z texture op 0, GEN_MODE bit 19 = 0, scissor offset 0x2ACAB (zero offset), clip XF 1005 = 0, line width irrelevant.
- Non-default: BP 43 = RGBA6_Z24 with **early Z (ztop=1) on 45 198 primitives**, late Z on 8 667. Early Z with alpha test writes depth for discarded pixels on hardware; Vulkan discard does not. EFB is 6 bits per channel on hardware.
- SU 0x30 flag bits set on 429 primitives (value 0xC on 330, 0x1 on 99).

Tracked as TD-049, narrowed to BP 43 and the SU flags. Texgen (TD-036) stays first by visual impact.

## Fog/indirect state without FIFO decoding (2026-10-15)

Final-review finding: `observe_bp` decoded BP words from every FIFO write in `uc.exe`. Replaced by call-level HLEs of the RBUE08 SDK setters, bit layouts taken from their disassembly: `GXSetFog` (80287DC4), `GXSetTevIndirect` (802872A4, 9th/10th arguments from the caller's stack), `GXSetIndTexMtx` (8028730C), `GXSetIndTexCoordScale` (80287460) and `GXSetIndTexOrder` (80287564), the last two keeping their SDK shadows (0x178/0x17C, 0x170 and dirty bits 0x5FC). Each writes the SDK's FIFO words unchanged (display lists stay byte-identical) and sets native state from its arguments; `GXSetTevDirect` uses the same path. Raw BP words in the FIFO no longer change native state.

Evidence: `test_normal_matrix_hle` (words and recorded bytes for every setter, orthographic fog B words, display-list replay, raw words ignored) RED (missing HLEs) → GREEN; full build 89/89. Live: train fog words `EE036995 EF400035 F0000002 F183CF4A` equal the Dolphin train capture; `GX_FOG_NONE` words equal Dolphin's title capture; title background animates; train scene renders. Cost: no measurable difference expected — the removed check ran once per FIFO write.

## Live texgen on the GPU — TD-036 (2026-10-15)

Dolphin capture probe (`build/probe/texgen_probe.py`, `post_probe.py`): the train uses only regular texgen — UV0..2 with ST (most draws), normal and position sources with STQ/ABC1 (≈2 200 primitives, projection-style matrices), dual-texture post matrices always on (rows 0/3/6 mostly identity, some with translation; rows 3/6 with normalize). No per-vertex texture matrix indices.

The vertex shader now evaluates exactly this from the raw XF words (`generateTexcoord` in `renderer/shaders/smoke.slang`); the HLE passes texgen/post words and three-row texture and post matrices. Tests: `test_texture_matrix_hle` (third row, post matrix 64+row, raw words), `test_gx_transforms` (model-space normal, vertex layout), `test_fifo_stream` (offline uniforms), `renderer_smoke` STQ-from-position fixture (perspective-correct Q) and normal-source normalize+post fixture (mutation without normalize fails). Full build 89/89, Vulkan validation clean.

Offline replay through the same shader: train 3.653983/255 and title 1.447028/255, identical to the former CPU bake. Live: `train_route.py --native-controls` passed end to end (exit 0) for the first time; first gameplay frame `frame-1792018487618603-21076-7729.png` matches the Dolphin train reference in overall brightness (ceiling, seats), where an earlier build's frame at a similar moment was much darker. No unsupported texgen reported.

## Same-frame parity: GX pixel centre 7/12 — TD-044 (2026-10-15)

Investigation of the remaining train error (mean 3.654/255): 60 % comes from small differences everywhere, 40 % from 12 080 pixels with >20 difference, concentrated as speckle in high-frequency textures (seat fabric, luggage nets, carvings) plus the lamp halo. Ruled out with evidence: CMPR decode (our 5/8 weights and 565 bit replication reproduce Dolphin's dumped textures exactly, `build/probe/cmpr_probe.py`), mip/filter selection (equal local Laplacian energy), display-copy vertical filter and RGBA6 quantization (applying them offline raised the error to 4.7–4.9), early Z (TD-049). Low-pass of both images (radius 2) drops the error to 1.58, so most of it is sampling phase.

Root cause: without AA the GX rasterizer samples at pixel centre 7/12, Vulkan at 1/2. Sweep of a viewport offset through `uc_fifo_replay` (train/title): -1/16 → 3.004/1.061, **-1/12 → 2.917/1.002**, -1/8 → 3.003/1.153, -1/6 → 3.288/1.380, +1/12 → 4.856/2.110 — minimal exactly at the hardware value. `uc::efb_viewport` now shifts every draw viewport (and the default full-EFB one) by -1/12 EFB pixel at any render scale; `test_render_scale` RED→GREEN, renderer_smoke --validation passes.

Remaining low-frequency difference (≈1.6/255 after low-pass): native slightly brighter (mean signed +0.27/+0.25/+0.13 RGB) and the lamp glow halo spreads less (bloom from scaled EFB copies, TD-045).

## EFB copy chain and integer TEV — TD-044/TD-045 (2026-10-15)

Stage comparison: `uc_fifo_replay --dump-efb dir` saves the EFB each texture copy reads; `scripts/dolphin_reference.py --dump-efb` enables Dolphin's DumpEFBTarget. After resampling our anisotropic replay EFB (640x640 for 640x480) the bloom chain copies 7–16 match Dolphin's copies at 0.01–0.09/255 — the glow downsample/blur is correct. Scene copies 3–6 differ by ≈1.1 (half resolution), copy 17 by 1.6; the error then grows in draw `e95bc`, a four-stage TEV colour grade of copy 17 (stage 2 computes (prev − konst)·2), which doubles any arithmetic difference. Draws after copy 18 are HUD only.

Root cause: TEV evaluated in floats without per-stage quantization. Now integer as in hardware: A/B/C use their low 8 bits, C extends to 0..256 (c + c>>7), D signed 11-bit, (d<<8 ± lerp + bias<<8), scale shift, +128, >>8, clamp. Rounding variants measured on train/title: +128 after scale **2.663/0.935** (changed pixels 161 898/125 902), sub-dependent 127/128 2.666/0.935, none 3.991/1.984, rounding the lerp before D/scale 3.179/0.935. Previous float path 2.917/1.002 (255 158/172 381 pixels changed). GPU fixture: an s10 register 1.5 read as input A gives 126 (low 8 bits); fails with the float model, passes now. Full build 89/89; live route passed; GPU render time unchanged (≈4.7–5 ms per presented frame).

## Scene residual and VI blanking — TD-044/TD-034 (2026-10-15)

Scene copy (before bloom) vs Dolphin: 1.54/255 at half resolution, interleaved ±1–3 speckle on edges and textures with a +0.36 brightness bias; RGBA6 truncation applied to our scene raises the error to 1.96 (bias −0.90), so it is not the cause. Left as sampling noise for now.

GXCopyDisp probe (live train route, distinct control/caller pairs): every display copy is a plain full-size copy (control 0x52004003/0x52004803, no half scale or intensity); the per-frame path at 8003A6EC copies, waits GXDrawDone, then VISetNextFrameBuffer + VIFlush. Routine 80039C50 blanks VI (VISetBlack(1) at 8003A248), copies the stale EFB with clear into one XFB, copies the cleared EFB into both XFBs and releases blanking (VISetBlack(0) at 8003A43C); 8003B1A0 blanks again around loading. On hardware those frames are black; the port presented each copy. VISetBlack now keeps the blanking state (released by default so a missed release cannot blank the port permanently) and a display copy taken while blanked presents black. Tests: test_vi, test_normal_matrix_hle (black/normal packets), renderer_smoke (black presented over an EFB draw). Live: gameplay frames normal; boot without --skip-intro shows the strap screen. Full virtual-XFB selection by VI address (TD-034) is still not implemented; presentation remains at copy time otherwise.
