# Native scene-pass diagnosis

Profile mode captures at most24batches starting with the first native mesh callsite80366E14. Each batch logs serial/draw count/copy extent, last draw material/colors and saves `S:/Games/Umbrella Chronicles/debug/pass-<pid>-<pass>-<serial>.png`. Main blocks subsequent render submissions until the batch fence and readback complete. This is native Vulkan diagnostics; no graphics-command interpreter or device model. Initial capture/readback/PNG overhead is intentional and must be excluded from performance comparisons.

`VulkanRenderer::capture(serial,true)` reads the current full EFB, including pixels outside the display rectangle. It requires the latest submitted serial and a completed fence. Old serials are rejected because later batches may modify the shared EFB. Buffer sizing follows EFB extent; ordinary screenshot capture still reads the retained presented image. EFB captures happen after that batch's copy/clear: a present-and-clear batch therefore shows cleared EFB, not its presented image. GPU regression verifies partial green clear/red remainder after nonpresent batch58and rejects stale capture58after59. RED old implementation rejected nonpresent capture; GREEN full GPU smoke/validation passes.

Visible internal-input run10612, log `build/autotest/1791894834451073900/run.log`, isolates the severe white output before postprocessing:

- Pass1/8468:359native draws, full scene copied640x360→320x180, clear0. `pass-10612-1-8468.png` already has white chairs/ceiling/light clipping, with recognizable floor/door/wood textures.
- Pass3/8470: one two-stage overlay/composition draw; the defect persists.
- Pass16/8483: final additive composition; clipping increases. This pass is not the original cause.
- Pass19/8486: presentation followed by clear; its EFB capture is intentionally dark.

Found a separate verified UV bug: native UV generators passed(s,t,0,1)to texture matrices, dropping their third-column contribution. They now pass(s,t,1,1). Semantics reference: [Dolphin VertexShaderGen.cpp](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/VertexShaderGen.cpp), UV source setup/AB11 homogeneous input. No external code imported. GPU fixture samples a red/blue texture atU=.1with matrix third-column offset.75: RED red output, GREEN blue output. Current UV-only generator scope is unchanged; position/normal/post/projective generators remain TD-036.

This test exposed a build defect: changed SPIR-V was generated, but executable POST_BUILD copies ran only when the executable relinked. uc_vulkan now exports shader files as INTERFACE_LINK_DEPENDS; both game and renderer_smoke relink/update runtime shader files when SPIR-V changes. SHA256matches across build/renderer/shaders, build/renderer/data/shaders, build/game/data/shaders anddist/data/shaders. Full61/61CPUtests/build/install pass; GPU smoke/validation passes. Scoped readback/diagnostics review: no findings.

Post-fix visible internal run49108, log `build/autotest/1791895118344888900/run.log`: `pass-49108-1-8396.png` still has the same severe white scene. The UV bug is fixed independently; it does not resolve the primary exposure defect. Final `frame-1791895189480442-49108-18647.png` shows reload tutorial, so route measurement8.88presentations/sec(2–15)includes pauses/stalls and is not active-gameplay acceptance. Later log intervals return14–15. Performance improvement is unclaimed.

Next evidence required: compare per-mesh vertex colors, actual bound lightmap format/data and selected texture matrices in the first geometric pass. Recorded texture-matrix inheritance is still a known independent gap. Scene parity and performance plan remain open. No commit/deletion; owner saves remain untouched.
