# Native geometry resources — partial renderer milestone

2026-10-12. Source: RBUE08 DOL and current native runtime. No recognizable game-image acceptance.

## Boundary and nativity

Verified GXCallDisplayList80288684 takes guest address in r3, byte length in r4; its dirty flush80284D58 and optional dummy flush8028511C precede the call. It now invokes owned native resources produced by GXBeginDisplayList/GXBegin/GXSetArray/GXEndDisplayList. Generated mapping has100HLE boundaries.

SDK entry points construct three typed host data items: geometry with validated VCD/VAT and attribute payload, array binding, and child-resource reference. Playback traverses these native objects and produces owned DrawPackets using the existing vertex decoder. It never reads a FIFO opcode or executes list bytes as commands. The retained guest buffer is compared only to reject stale/out-of-band resource changes. Raw GX state words remain untranslated into native materials; TD-014 stays open.

Indexed array data and child-resource revisions resolve at call time. Recording changes no live array binding; nested resources preserve binding order. Import validates the whole resource graph before publishing any draws/bindings. Cycles and nesting beyond64 are explicit sequence errors. Successful overlapping recording invalidates stale cached identities; overflow returns zero and preserves the prior destination/cache. Unknown/prebuilt or externally modified lists stop with an address and typed layout error; they require a verified native asset boundary.

## Verification

RED: missing hle_GXCallDisplayList link target. GREEN: direct and indexed SDK recordings, arrays changed after recording, queued packet ownership, child rerecording after parent creation, mutated/missing/changed-size resources, cycle rejection and no partial publication. Existing primitive-format validation is reused through encoded_draw_size without allocating temporary vertices while recording.

Final12-second validation run exits0:721retraces,19opens/63reads/13637056bytes,7030nonempty resource calls, **730native draws /2920vertices**,1420nativeGPU submissions /1419completions, zero unmappedMMIO and no VUID observed. Shutdown can cancel a pending final batch; timer counts and GPU submissions are not displayed-FPS evidence. Every live draw contains game-produced attributes; no synthetic geometry substituted.

Mid-item gpt-6-luna review reported no findings. Whole-plan Astra review remains reserved for completion of all tasks.

Complete build.bat passes50/50tests, stages dist and copies to <install dir>. Copied exe started from C:/Windows/Temp with redirected stdout/stderr, six-second validation exit0:354draws/1416vertices,667GPUsubmissions/666completions,361retraces, zero unmappedMMIO/VUID, all guest/file workers joined. Final pending graphics work may be canceled at shutdown; no completion fabricated for it.

## Remaining work

Game transforms/projection, GPU textures and sampling, channels/texgen/TEV/depth/blend/clipping, EFB/XFB copies and prebuilt/inlined-resource imports remain pending. Current smoke shader cannot establish scene parity. TD-032 tracks full-buffer validation and repeated native attribute conversion until profiling and resource-version caching replace them. SDL direct-tool-console startup limitation remains TD-031; normal interactive launch has not been verified.
