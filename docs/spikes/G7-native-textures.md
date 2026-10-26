# Native texture binding and first recognizable startup image

2026-10-12. User confirms the EFB fix removed flicker, leaving a white screen. The previous shader consumed only vertex color. Native texture upload/sampling now produces the Nintendo Wii wrist-strap instruction card from the game's resources; later GPU readback after120presents was inspected. This is an actual rendered startup image, not a reference image substituted for missing output. Menu/gameplay and full graphical parity remain unverified.

## Verified SDK boundary

Independently disassembled DOL8028643C/GXInitTexObj and802869A0/GXLoadTexObjPreLoaded. The investigator's initial offset summary was inconsistent with disassembly and was not used as implementation evidence.

GXTexObj: +0mode0, +4mode1, +8image0 (width−1 low10bits, height−1 bits10..19, format bits20..23), +0Cimage3 (address in32-byte units, low24bits), +14full format, +18TLUT id, +1Fflags (bit1 direct/non-TLUT, bit0 mipmapped). Region argument r4 is a texture-cache region, not a TLUT object. PreLoaded takes object r3, region r4, map r5; GXLoadTexObj remains translated, preserving its cache-region callback at SDK+518.

PreLoaded writes six words in order: object0/4/8, region0/4, objectC. Their top bytes come from SDA tables r13−28936,−28928,−28920,−28912,−28904,−28896 indexed by map. Native HLE retains exact checked SDK bytes for resource-version checks; it does not decode/execute BP commands. Object/region words and SDK+564/+584 per-map shadows, dirty bit1 and SDK+2 are preserved. All eight binding slots are accepted because GX initialization populates them before drawing. The initially narrower map0-only call boundary was rejected by real startup and expanded; GPU material routing remains a separate pending step.

Recorded bindings own descriptor values. Texture data is read when a native resource is played, then decoded into immutable owned RGBA data attached to DrawPacket. A later guest overwrite cannot change an already published draw. Replaying the same source address observes updated data. Storage sizing uses the same validated tile/mip rules as decoding; CI/TLUT and unsupported formats fail explicitly.

## GPU path and tests

The native vertex stream now includes UV0 (72bytes). Vulkan uploads owned RGBA data, uses checked descriptors and TRANSFER→FRAGMENT_SHADER barriers, and retains image/staging/descriptor resources until the slot fence completes. A DrawPacket without a texture uses a white identity sample; verification of which GX bindings a material actually requires awaits TEV routing (TD-036). Malformed uploaded images are rejected.

RED compilation established missing binding interfaces. GREEN HLE tests cover map bounds, eight-slot initialization behavior, SDK shadows, out-of-RAM address, recorded-byte identity, descriptor/data version behavior and immutable queued textures. Real Vulkan fixture checks distinct2x2texels by UV, replacement and reuse across two slots, truncated RGBA rejection, unsupported mip rejection and clean recovery/shutdown with ordinary validation. Eight GX-targeted CPU tests pass.

Six-second game trace: exit0,361draws/1444vertices,688finish submissions/687reported completions,349copies/348presents, no unmappedMMIO or ordinary-validation errors. Early readback remains black; later serial349 is the correctly recognizable wrist-strap card. Last copy/present count can differ by one at bounded shutdown.

Final full release build passes51/51tests, stages dist and copies to <install dir>. Installed GPU fixture launched from C:/Windows/Temp passes ordinary validation. Installed release game runs20seconds at3840x2160 with ordinary validation: exit0,1200retraces,19opens/63reads/13637056bytes,1206draws/4824vertices,2372finish submissions/2371reported completions,1191copies/1190presents, zero unmappedMMIO; all workers joined. Separate trace readback confirms actual3840x2160 output. This verifies startup rendering/lifecycle for a bounded run, not playability or title/menu transition.

One scoped Luna review suggested replacing CMPR5:3 interpolation with2:1. Rejected after rechecking Wii-specific documented behavior and existing literal fixtures: [Dolphin authors' conversion-shader documentation](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/TextureConversionShader.cpp) describes3/8 and5/8 weights. No external decoder code copied.

TD-036 records the limited map0/UV0 nearest/clamp single-mip modulate shader, pending verified TEV, texgen/channel/sampler/depth semantics. TD-037 records uncached CPU decode and per-batch upload/resource allocation. TD-035 synchronization-validation/overlay isolation stays open. No interpreter, JIT, PowerPC runtime or graphics-device model added. The renderer plan remains incomplete.
