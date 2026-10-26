# Native position matrices and projection — partial renderer milestone

2026-10-12. First recognizable game image remains pending.

## Verified SDK boundaries

Local RBUE08 DOL verifies GXSetProjection8028873C (matrix pointerr3/type r4), GXSetProjectionv802887A0 (type float followed by six coefficients), GXLoadPosMtxImm8028882C (twelve floats/r4 row), GXLoadPosMtxIndx8028887C (r3 source index/r4 destination row), GXSetCurrentMtx80288934 (low6bits in SDK+0x80). Projection shadow is SDK+0x528/type,+0x52C..0x540/coefficients; projection/current dirty masks0x08000000/0x04000000 are preserved.

Five Native HLE boundaries replace these calls; total105mappings. Position loads own float rows, indexed loads read current GX_POSMTXARRAY binding at playback, and projection/current changes participate in the typed native resource graph. SDK shadow restoration does not submit recorded state to live GPU state. Direct draws and nested resources snapshot owned transform state before entering the renderer. Unknown/out-of-range matrix rows and nonfinite coefficients are explicit typed errors; no register/device model or encoded FIFO opcode execution added.

## GPU path

Vertex shader performs position-matrix multiplication and perspective/orthographic projection. Projection uses64bytes of vertex-stage push constants. Selected position rows accompany each vertex in a compact64-byte GpuVertex (position/RGBA/three float4 rows), supporting different position indices within one draw. Arithmetic stays on GPU; CPU validates/selects/copies rows. TD-033 tracks the cost of repeated row data until profiling justifies a shared GPU palette.

Projection is rebuilt from the SDK's six coefficients. GX clip depth-w..0 maps affinely to Vulkan0..w; Y flips for the current positive-height Vulkan viewport. No OpenGL half-range conversion or scene-specific projection bypass added. Fact references: authors' [GX clipping research](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/VertexShaderGen.cpp) for the console range, [Khronos push-constant specification](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdPushConstants.html) for pipeline-layout/range requirements. Implementation and shader written independently; no external renderer source copied.

## Verification / visual limit

RED: missing transform interfaces, missing HLE matrix methods, then actual GPU center-pixel failure while the old shader ignored matrices. GREEN: pure fixtures verify identity/translation/per-vertex selection/row bounds/nonfinite rejection, orthographic and perspective near/far mapping. ABI fixture verifies full projection offsets, SDK mirrors, owned immediate matrices versus deferred indexed sources, current-matrix snapshots and nested recording. GPU smoke verifies two position matrices, perspective then orthographic projection, pixel colors, resize/zero extent, two-slot completion order and shared PNG output; native readback inspected.

Twelve-second real run:730draws/2920vertices,1420submissions/1419completions,721retraces, no unknownMMIO/VUID and exit0. Six-second audit with readback:367draws/1468vertices,696submissions/completions,360retraces; capture serial19 is1280x720. The first geometry frame is **black**, not a logo/menu/scene acceptance. Texture/TEV/blending/depth and copy state are unfinished; the black frame alone does not identify a new bug or establish scene parity.

With trace --gx-audit, the first completed frame containing game geometry is saved beside the executable as gx-first-geometry.png, after its real fence completes. PNG writer reused from renderer_smoke, validates dimensions/data before opening output and balances COM lifetime. Retained historical PPM artifact was not deleted. Normal release has no audit/readback work enabled.

Scoped mid-item Luna review reported no findings. Remaining Task6 work: normal/texture matrices, channels/texgen/TEV, depth/blend/clipping/raster and viewport/depth-range fidelity. GPU texture binding/sampling and EFB/XFB copies also remain open; first game-image/4K/temporal acceptance unchanged. SDL direct-tool-console startup remains TD-031.

Complete build.bat:51/51CPUtests passed; dist staged and copied to <install dir>. Current GPU smoke with shared PNG writer also exits0 under explicit Khronos validation. Native source/diff check passed; normal interactive launch remains unverified.

Installed exe started from C:/Windows/Temp with redirected output:6seconds exit0,370draws/1480vertices,699GPUsubmissions/698completions,361retraces, all workers joined, no unknownMMIO/VUID. A final pending batch can be canceled at shutdown; no completion synthesized for it.
