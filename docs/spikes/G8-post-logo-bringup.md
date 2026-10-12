# Post-logo startup bring-up

2026-10-12. User confirms startup logos advance and reports a crash near the expected intro movie. Checkpoint commit: `8405591` (51/51 CPU tests passed before commit).

## Missing static entry

Reproduction: `scripts/bringup.bat --seconds 90 --validation --gx-audit --log-dvd`, with ReShade disabled only for the child process. Exit3 at2661retraces (about44seconds),19DVD opens/63reads, zero unmappedMMIO. Dispatch traps on unknown entry `80112D68`; no movie file has opened yet. The symptom alone therefore does not establish a codec failure.

DOL disassembly at80112D68: `lis r4,0x8010; addi r4,r4,0x5764; b 0x8009E36C`. The previous instruction ends another thunk with an unconditional tail branch. Add the complete three-instruction callback-registration thunk as an explicit static entry in `project/uc.cfg`. This adds a compiled function, with no runtime instruction decoding or compatibility stub.

The next run passes80112D68 and traps on80112E58, another thunk in the same block. Complete disassembly verifies231 consecutive12-byte functions from80112D08 through801137D8, each exactly `lis r4; addi r4,r4; b 8009E36C/8009E450`. The block follows a `blr`;801137DC starts a different six-instruction initializer. All231 entry addresses are now explicit in the static translation config, including the previously added80112E1C. This is a bounded DOL-specific discovery correction; no speculative entries outside the verified block.

The block fix reaches the next genuine renderer gap: `native TLUT binding is pending (TD-036)` at2714retraces. Still19opens/63reads; movie playback has not started.

## Native palettes

Parent independently verified DOL80286B58/80286B80/80286CC8 and80286A80..80286AB0. The investigator identified the preload callback but did not resolve the other boundaries; its incomplete report was not used as ABI evidence.

GXInitTlutObj writes palette format in object+0 bits10..11, source address in object+4 low24bits in32-byte units, and entry count as big-endian u16 at+8. GXLoadTlut(object r3,name r4) calls SDK+51C(name), flushes texture state, emits object+4 and region+0, flushes again, merges the region's low10bits into object+0, then copies object0/4/8 into region4/8/C. PreLoaded calls that same callback with texture object+18 and tags region+4 using the map table at r13−28888.

Native GXLoadTlut preserves these SDK side effects and callbacks. Each logical SDK region owns palette bytes copied at load; queued draw packets own decoded RGBA. Recorded palette loads own source descriptors, read current source bytes at native-resource playback, and publish state transactionally with the rest of that resource. No TLUT hardware memory or command processor is introduced. Distinct SDK region objects aliasing the same physical TLUT range are outside this logical-resource model; revisit with native shared resource identity if observed.

CPU decoder covers C4/C8/C14X2 and IA8/RGB565/RGB5A3 palettes. Literal fixtures verify nibble order, edge tiles, high index bits, alpha and short palettes. HLE fixtures verify owned load data, palette replacement at playback, immutable earlier packets and SDK shadows. Both tests first failed for missing interfaces, then passed. Format interpretation cross-checked against the [Dolphin authors' texture-format implementation](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/TextureDecoder_Generic.cpp); no implementation code copied.

Trace validation run now passes90seconds, exit0:5395retraces,61244draws/244976vertices,6373finish submissions/completions,3196copies/presents,1112GXLoadTlut calls,19DVD opens/63reads, zero unmappedMMIO and no ordinary Vulkan validation errors. All guest/file workers join. The original entry traps and explicit TLUT failure are resolved. No video file opens in this run; movie playback and post-logo visual parity remain unverified. Input is still pending (TD-013), and the bounded run does not prove title/menu progress.

One scoped Luna review found no issues. Added explicit HLE checks for out-of-RAM palette data and zero/oversized entry counts; tests pass and invalid loads preserve SDK palette shadows. TD-036 updated, TD-038 records the logical SDK-region alias limitation. No extra commit after the requested checkpoint.
