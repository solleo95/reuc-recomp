# Native internal render resolution

Owner goal includes4Kand higher image quality. Optional default-choice question pending; after reasonable reply window, proceed with recommended automatic scale and manual override. Existing EFB inherits first window extent and remains unchanged on resize; GPU copy versions stay at original logical resolution, reducing postprocessed scene detail.

1. CPU RED tests: automatic integer scale for640x480/1280x720/1920x1080/2560x1440/3840x2160, bound1..6, CLI --render-scale auto|1..6 and invalid values.
2. GPU RED tests: canonical640x480EFB multiplied by scale, scaled immutable copy targets, readback actual EFB dimensions, viewport/scissor/clear/present cropping and old-copy ownership across scale changes.
3. Rebuild EFB/depth at new scale only after device retirement. Keep logical guest descriptors/copy sizes; allocate scaled physical GPU copy images and mips, validate device limits. Preserve CPU textures and normalized UV behavior. Bounded recycle budget may scale up to512MiB with internal scale; copy count remains32.
4. Build/full tests/Vulkan validation; audible visible defaultHDtrain and4Kcapture with internal input/isolated saves. Confirm screenshots have genuine high-resolution geometry, not only enlarged final blit. Measure actual FPS/allocation/memory behavior; keep STQ/indirect/fog and arbitrary FPS out of this item.

No emulator/device model, host input interception, save mutation, deletion or automatic commit.
