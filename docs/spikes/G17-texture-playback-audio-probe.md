# Dynamic native texture matrices and audio evidence

Checkpoint927e48f records the previous native train/audio/material/cache work. Subsequent changes remain uncommitted.

## Texture playback

Recorded draws froze texture matrix rows, unlike position matrices/live colors. New RED regression proves external matrix changes were lost. Draws now own selector IDs; typed TextureTransform operations own loaded values; nested playback stages the matrix bank and commits only after complete validation. Shared row resolution preserves identity selector60 and current matrices for immediate/mesh/recorded draws. All twelve3x4input floats validate before publication; STQ Q/postmatrix semantics remain deferred.

Tests cover live inheritance, explicit ownership after source mutation, later live inheritance, nested ordering, failed-child rollback, identity and invalid third-row floats. Initial map-copy staging regressed to22–26FPS; fixed bank126optional matrix slots removes per-call allocations while preserving staged copies. Selectors are masked<=63 and loads validate<=125. Full63/63tests/build/install and Vulkan smoke/validation pass. Scoped texture review and storage-refinement recheck: no findings.

Visible PID9764, log `build/autotest/1791906852671413500/run.log`, shows proper animated characters/train/HUD in `frame-1791906912200576-9764-27358.png`. Active battle intervals24–30FPS with29–30early, later3-character/effect load drops. Death-screen60FPS excluded. This is not an FPS-unlock or constant30FPS claim.

## Audio probe

Normal Release --profile now logs at most64non-streaming voice starts/whole-voice ends; probe state resets during audio initialization. No full guest trace required. Controlled visible tests use an actual SDL output stream with gain0, preserving queue/device timing without audible diagnostic audio. Counters are atomic; normal manual launch remains audible.

Dummy-device regression verifies delivery counters and overrun clearing. Actual PID10708 log `build/autotest/1791906641897193500/run.log` maps confirmation sequences10/14/18 to ADPCM voice starts at frames3449/4606/5490, each whole-voice PCM peak8391. SDL queues remain below2204bytes and overrun count0 during these menu presses. Therefore these confirmations produce nonzero native PCM and are not dropped by the queue in this run. Audibility/subjective correctness still unverified; do not claim all button sounds fixed.

Loading/battle eventually reaches the existing100msqueue threshold, producing9overruns in PID10708and10in PID9764. Drift/overdue scheduling/queue-clearing policy remains TD-042. Mixer-control routing was checked against primary [AXWii ConvertMixerControl](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/HW/DSPHLE/UCodes/AXWii.cpp): bits1/2enable mainL/R; bit4enables both plus ramps. Current main-channel masks agree; no speculative channel-mask change made.

Owner-supplied Gecko FPS lead and verified original branch are recorded separately in F0-fps-unlock.md. No patch/interpreter enabled.

Owner now explicitly requests audible controlled tests. Native controlled launch opens the ordinary audible SDL stream; internal input/isolated-save behavior is unchanged. Prior gain0probe results above are historical. Future diagnostic windows play game audio so the owner can assess it during autonomous runs.
