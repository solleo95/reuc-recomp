# Native EFB image allocation reuse

Actual train creates approximately500–600GPUtextures/sec with CPU decode count0. These are fresh immutable copy versions with identical image extents/mip counts. Renderer destroys each expired version after all owners/fence references disappear, then allocates another image/device memory/view.

1. GPU regression: release completed copy versions, overwrite through new owned versions, then prove warmed allocation count stops growing. Retained old versions must still sample original pixels, including two-slot completion and resize.
2. Recycle only copy images whose map is the sole source owner; slot source references hold all active GPU uses through fences. Reuse matching validated extent/mip count with undefined layout/discard, new immutable source and initialized=false. Bound free storage by actual allocation bytes(64MiB)and count(32). Release free images on shutdown; never reuse a still-owned version.
3. Validate malformed chains even on reuse, plus budget/shape changes, clean shutdown/Vulkanvalidation. Full tests/build/install and audible skipped-intro train profile; separate image allocation rate from actual FPS gain.

This is native Vulkan resource pooling, not device emulation or a command interpreter. Physical input and owner saves remain untouched.
