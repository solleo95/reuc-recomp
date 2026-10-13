# FPS unlock lead supplied by owner

Owner supplied Dolphin/Gecko code `04158204 60000000` as a future lead; do not apply automatically to other regions/builds.

Code interpretation follows [Gecko codetype documentation](https://wiigeckocodes.github.io/codetypedocumentation.html): a32-bit write at base0x80000000plus offset0x158204, assuming the standard base.

For this RBUE08 DOL, address `0x80158204` contains `0x4180FFF0`: conditional backward branch to `0x801581F4`. Proposed replacement `0x60000000` is PPC NOP (`ori r0,r0,0`). Native implementation should change the translated pacing boundary or corresponding source logic; no runtime Gecko/PPC interpreter.

Disassembly: function `0x801581E0` waits by calling `0x8026E194`, compares object fields `+0x20` and `+0x28`, then stores/clamps elapsed count into `+0x24` (maximum5), resets `+0x20`, and continues timestep math. This strongly suggests a frame-count wait; 60FPS behavior and timestep correctness still require runtime measurement. Removing the wait alone must not be assumed to provide arbitrary FPS or correct simulation speed.

When working on FPS: verify DOL identity and original instruction; trace the counter producer and configured target; compare native simulation/wall-clock speed, camera/events, audio/video timing and loading against baseline. Then implement fixed simulation cadence plus independent presentation/interpolation where needed. Test60 before120/144/240/unlimited. Regional images must use verified symbols/patterns, not this fixed address.

Status: original branch confirmed with xref; no instruction patch applied. Opt-in native local-wait experiment now tracked in F1-native-60fps.md; original mode remains default.
