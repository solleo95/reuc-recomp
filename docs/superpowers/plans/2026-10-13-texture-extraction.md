# Offline texture extraction

Goal: extract the game's stored GX textures without playing it, preserving RGBA,
original mipmaps, Dolphin names, and resource provenance. Texture replacement is
future work. Existing texture packs must remain untouched.

Implementation runs in the current checkout; unrelated input-binding changes
remain owned by their existing author. No commits or publishing requested.

1. Add fixture tests for nested U8 archives, TPL descriptors/palettes/mips,
   BRRES TEX0/PLT0, font sheets, XXH64 names, corrupt input and output protection.
   Run them before implementation; expect the missing extractor assertion.
2. Add `tools/texture_extract/extract_textures.py`: standard-library parsing,
   XXH64, lossless PNG, deterministic manifest and coverage report. Use a small
   native DLL wrapping the existing `gx::decode_texture`; avoid a second decoder.
   Validate every offset, count, format, size and recursive container boundary.
   Surface failed resources and unsupported texture containers; nonzero exit on
   errors. Scan all input files, including embedded resources in the DOL/banner.
3. Register DLL, Python script, Windows launcher and tests with CMake. Default
   output is `TexturesDump/RBUE08`, separate from the existing DDS pack. Repeated
   extraction verifies existing output rather than overwriting edited PNGs.
4. Build, run fixture tests and project CTest suite, extract actual game data,
   inspect representative PNGs, compare names with existing Dolphin textures,
   and report counts and precise coverage limits. Final independent review.

Review focus: archive names are metadata only (never output paths); alpha must
survive PNG encoding; malformed counts cannot allocate unbounded memory; missing
palettes must fail; same Dolphin name with differing pixels/mips must stay visible
in the report. Offline extraction cannot include textures generated during play.

Sources: Dolphin TextureInfo.cpp (XXH64 base-level and palette-range hashing),
devkitPro libogc tpl.c (TPL layout), RiiStudio TextureIO.cpp (TEX0 layout),
NintyFont formats/RFNT/tglp.cpp (font sheet layout). Parsers written independently.

## Verification ledger

- Tasks 1–3 complete: existing GX decoder linked through a checked C boundary;
  U8, TPL, BRRES dictionaries, font sheets, effects, compressed banners and DOL
  resources implemented. Native DLL, launcher and script installed separately.
- Task 4: actual DATA inventory and extraction agree on 16,490 stored resources:
  10,744 TEX0 + 5,111 TPL + 283 REFT + 341 font sheets + 10 banner + 1 DOL.
  Export: 8,205 unique encoded resources, 17,814 PNGs, no errors. All PNG payloads
  independently decompressed; all manifest paths present. 7,950 filenames match
  the existing Dolphin DDS pack. Thirty resource occurrences require the palette
  wildcard because padding references unsaved TLUT bytes; visible pixels decode
  successfully. Both sampler-name variants remain intentional.
- Final Astra review found one P2: a conflict/corrupt member stopped its outer
  archive and truncated provenance. Two regression tests reproduced this (RED).
  Per-texture and per-member error boundaries fixed it (GREEN). Fifteen extractor
  fixtures and all 70 current CTest tests pass; unrelated input work added the
  seventieth test concurrently. No commits requested or created.
