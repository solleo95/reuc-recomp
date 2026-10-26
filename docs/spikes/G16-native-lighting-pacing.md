# Native lighting and render pacing

Owner supplies `<install dir>/debug/reference.png` as a visual reference and authorizes continued autonomous visible testing with skipped intro/internal input. No host mouse/keyboard input or owner-save mutation.

## Root cause of white geometry

Actual first32mesh snapshots have no color0 vertex attribute, identity UV matrices and I8 lightmaps. The SDK channel cache still reports441/401with lighting disabled. That cache is stale: NW4R80376148loads channel resource data directly, bypassing SDK shadows. Prior G13inference that scene materials disable lighting is invalidated by this evidence.

Diagnostic native wrapper forwarding to AOT8037614C captures real40-byte channel resources. Scene records flags3F, materialFFFFFFFF, ambientFFFFFF00modulated by090909FF, color control702and light mask119. Lighting is enabled and uses material registers, not default white vertex colors.

Final80376148replacement is fully native: validates resource/masks/count, captures changes before publication, applies channel0ambient modulation with verified nearest integer rounding, mirrors material/ambient/control state into the native SDK shadow and preserves final NW4R generation-cache transitions.80373D00also has a native typed generation-mode replacement; SDK cull conversion table is read/validated, not guessed. The unused8037614Centry remains available for comparing the original static body; final path does not call it. No FIFO/register interpreter or device model introduced.

GXLoadLightObjImm now captures real64-byte light objects at the SDK boundary: color at12, cosine/dist/position/direction at16/28/40/52. Finite floats checked before publication; recorded Light operations own their input values and execute in order. GXLoadNrmMtxImm reads the first three columns of the3x4source; indexed mesh normal matrices now update actual native transform snapshots. Position/normal matrix boundaries split native batches. Normal vectors are transformed during native GPU upload preparation; lighting evaluates in the Vulkan vertex shader. Missing normal rows retain an identity fallback, recorded separately as fidelity/performance debt.

Vulkan has two raster color channels, independent RGB/alpha material/ambient controls, eight actual lights, signed/clamped diffuse and no/specular/directional/spot attenuation. Color-channel count0emits zero raster output. Integer accumulation/material scaling preserves tested8-bit results. Cull none/back/front/all uses native material state and clockwise front faces. Semantic references: [XFMemory.h](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/XFMemory.h), [LightingShaderGen.cpp](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/LightingShaderGen.cpp), [Vulkan raster state](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoBackends/Vulkan/VKPipeline.cpp). No external code imported.

CPU fixtures validate finite/bounded normal matrices, real channel control/mask/ambient publication, disabled-lighting bit, generation-mode rejection without mutation, recorded light ownership/live inheritance and immutable normal snapshots. GPU regressions: ambient9with full material yields8(integer scaling), ambient+128light yields137, zero channel count yields black, cull-all preserves clear pixels. Full61/61tests/build/install pass; GPU validation passes.

Visible23128first pass `pass-23128-1-7898.png`, then19164`pass-19164-1-8129.png`, show dark wood/ceiling, localized lamp illumination and readable cloth instead of the white scene. White/translucent highlights on some rear chairs remain; full scene/material parity not accepted. Presentation rate remains roughly14–15before pacing work. These are EFB pass captures; the area below the360-pixel display source is intentionally unused.

## Native wakeups instead of unconditional sleeps

Main previously slept1ms after every loop, even between short GPU passes; the scene produces around20copies per frame. Packets now signal a native condition variable after atomic publication; cancellation wakes waiters. Render loop waits for the oldest unfinished Vulkan fence(max1ms)or a new packet(max1ms), preserving frame-cap/minimize/idle behavior. No spin loop, fabricated completion or simulation-time change. Tests verify publication wakes a reader, cancellation returns false, GPU waits do not consume completion reports, and no work remains after retirement. Full61tests and GPU validation pass.

Visible52344run, log `build/autotest/1791897792705720800/run.log`, increases sampled intervals to19–20presentations/sec from14–15. Route fails on Windows WinError5while atomically replacing the control file held briefly by a reader, so no complete active-gameplay benchmark is accepted. Runner retries only sharing/access errors5/32for at most2seconds, retaining atomic replacement and increasing-sequence consumption acknowledgements; mocked retry/timeout checks pass. New12396run is pending final visual/FPS acceptance. Tutorials still have their own text gate; route now sends16confirmations with3-second settling. No commits/deletions.

## Review rulings and remaining checks

Scoped review identified zero-channel output; explicit shader channel-count gate and GPU regression added. Attenuation-enum finding was retracted after primary-source verification. Remaining image defects, exact normalization/degenerate-light behavior, absent-normal fallback, recorded texture matrices and exact LOD behavior remain debt. Controlled diagnostic audio is muted; actual audible synchronization has not been rechecked in these runs.

## Recorded channels, sampling and geometry reuse

Ordered native recorded channel-count/control operations restore HUD and tutorial text while retaining the zero-channel gate. PID43596 active battle intervals are 19–20 FPS; terminal route measurements on the death screen are excluded. Texture-cache address indexing preserves full source/palette-byte validation; 50,000 lookups across 512 addresses improve approximately42ms to0.7ms in the isolated benchmark, not a game-FPS claim.

Native Vulkan samplers now honor SDK mode bits S0..1/T2..3/mag4/mip5..6/min7. Point/linear and disabled/point/linear mip modes pass GPU fixtures/validation. PID44740 tutorial blur is smooth and HUD correct. Bias/clamps/aniso and exact hardware sampling remain debt. PID42036 bounded profile found no active unsupported position/normal/STQ generator in this route; UV-generator hypothesis not established.

Whole-mesh native cache validates encoded bytes, descriptor/active VAT/array bindings and referenced array-byte snapshots on every hit. Geometry owns vertices/indices; typed position/normal bindings resolve current pose on every call; materials/textures/viewport remain current. Storage bound32MiB with LRU eviction. Primitive-granularity trial regressed11–13FPS and was replaced with complete mesh-resource caching.

Visible PID13448, log `build/autotest/1791903788189148000/run.log`, confirms active battle28–30FPS from19–20, 230initial misses and steady hits without further misses. `frame-1791903855434563-13448-32480.png` confirms animated characters/HUD/train. 60FPS death-screen values excluded. Source/encoded mutation, new pose/viewport and failed-import regressions pass; full61tests/build/install pass. Scoped cache review no issues. White character/chair highlights remain; scene parity not accepted.

## Subsequent material investigation

Normal upload now selects palette rows through `position_index & 31`, including the shared current index. Regression with position33 and different normal1/33 matrices fails before the fix and passes after it. Semantics verified against [VertexShaderGen.cpp normal selection](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/VertexShaderGen.cpp). PID44436 still retains white areas; this correction does not establish their root cause.

Bounded mesh diagnostics now capture each new asset (maximum256), not repeated first32 draws. PID10296 has230assets/56distinct material signatures; every captured TEV color stage is clamped. Unsupported UV and unclamped-intermediate hypotheses remain unproven for this route.

Disassembly80367024 reveals a missing native color-material boundary: the ordinary r4=0 path sends a prebuilt128-byte resource call directly to FIFO, bypassing the owned SDK-call path. Those writes were counted/discarded, leaving stale native TEV/Konst colors. Typed ResColorApply now imports a fixed native asset layout: three20-byte signed register records, four padding bytes, four10-byte Konst records,24padding bytes. Validates tags/type/reserved bits/duplicate high words/padding before publishing. Signed11-bit register components preserve negative values; Konst components are bounded8-bit. Recording preserves the9-byte resource-call footprint but owns typed color values, including playback after source mutation.

Visible PID636 (`build/autotest/1791905299883693300/run.log`) accepts all actual color-resource layouts and restores character skin/clothing detail and dark cloth chairs. `frame-1791905359322909-636-27070.png` confirms severe white highlights are eliminated. Active sampled rates remain approximately27–30FPS; terminal route rates excluded. Clean shutdown, all13956batches retired, unmapped MMIO0. Parser malformed-field/padding, HLE late-field rollback and recorded ownership regressions pass; full61/61CPUtests/build/install pass. Scoped native color review: no issues. General fog/indirect effects/exact TEV rounding and higher rendering resolution remain future fidelity work; no full renderer acceptance claim.
