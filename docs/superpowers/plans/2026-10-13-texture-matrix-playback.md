# Native texture-matrix playback

Recorded draws currently freeze UV matrix rows. Preserve recorded matrix selection, inherit current matrix contents at playback, and execute explicit owned matrix loads in order, including nested resources.

1. Add failing HLE regressions: live inheritance, recorded ownership, later draw inheritance, nested ordering, failed-child rollback, identity selection and finite validation of all twelve 3x4 inputs.
2. Add typed texture-matrix operations and recorded selector IDs. Stage the matrix map during import; publish only after complete validation. Resolve rows through one shared helper for immediate/mesh/recorded draws.
3. Run targeted/full CPU tests, Vulkan smoke, build/install and a visible skipped-intro train route with isolated saves/internal input. Inspect scene/HUD and actual active FPS. Record limitations honestly; STQ/postmatrix semantics remain separate debt.

Audio/button investigation is independent and read-only until runtime evidence establishes the missing path. No device/FIFO interpreter, physical input, owner-save changes or further commit without user instruction.

Execution: RED frozen-row regression, GREEN inheritance/ownership/order/rollback/identity/fullfloatvalidation. Fixed126-slot native matrix bank avoids the initial map-copy regression. Full63tests/build/install and Vulkan validation pass. Visible9764train confirmed with active24–30FPS; stable frame pacing remains open. Audio probe confirms menu PCM and no queue drops at sampled confirmations; subjective sound unverified. See G17-texture-playback-audio-probe.md.
