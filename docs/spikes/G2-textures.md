# Native texture decoder — CPU baseline

2026-10-12. `renderer/gx_textures.cpp`, `tests/test_gx_textures.cpp`.

Implemented I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8 and CMPR. Input is encoded texture data; output owns RGBA8 pixels for each mip level. This conversion executes no guest CPU instructions and models no graphics device.

Literal fixtures cover tile rows and columns, partial edge tiles, nibble order, alpha/intensity order, RGB channel expansion, RGBA8 AR/GB planes, CMPR's four sub-blocks and MSB-first selectors. CMPR uses Wii's 5:3 interpolation; transparent selector retains the endpoint average RGB. Fixtures are written locally; no game assets or external implementation copied.

Each mip consumes full padded encoded tiles. Descriptor validation bounds dimensions to the SDK's ten-bit extent fields, rejects zero/oversized extents and mip chains past 1x1, and validates the complete input before output allocation. Tests cover truncated planes/mip chains and a 1024x1024 eleven-level image. Previously returned pixels remain stable when input storage is overwritten; a fresh decode sees changed data.

RED: decoder stub rejects the first fixture. GREEN: texture test and complete 49-test CPU suite pass.

Task 5 remains **partial**: GPU texture upload/sampling, game object bindings, content-aware cache and TLUT/indexed formats remain pending. Unsupported format IDs return a typed error; no placeholder texture. CPU throughput not profiled yet. There is no texture cache yet, so this test does not claim cache-invalidation correctness.

Format references: [YAGCD texture layout](https://hitmen.c02.at/files/yagcd/yagcd/chap15.html#sec15.20), [devkitPro GX format declarations](https://github.com/devkitPro/libogc/blob/master/gc/ogc/gx.h). Wii-specific CMPR behavior checked against the project authors' documented findings in [TextureDecoder_Generic](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/TextureDecoder_Generic.cpp) and [TextureDecoder_Util](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/TextureDecoder_Util.h); implementation here written independently from format rules.
