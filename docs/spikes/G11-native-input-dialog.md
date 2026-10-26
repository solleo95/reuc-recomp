# Native mouse and save-dialog rendering

2026-10-12. User requested raw mouse aim, LMB primary action, correct crosshair/button icons and diagnosis of the corrupted font. Extends the approved native input/GX boundaries inline; preserves ordinary dialog/save flow.

## Native input

Host SDL3 relative mode uses unscaled motion; system acceleration hint is forced0, speed scale1. The existing Windows SDL backend uses its native raw-input service. Relative counts integrate into a bounded pointer [-1,1] and publish through a mutex-protected NativeInput snapshot. LMB maps to Nintendo A0x0800. Trigger/release edges survive a short down/up between samples; each edge is consumed once. Losing focus releases held buttons, discards pending presses and marks pointer invalid. Escape releases capture; next LMB restores capture without accidentally selecting a dialog button.

Verified DOL: KPADRead802F21EC, sole game caller8003B558. Caller requests16 samples with132-byte stride; position is at sample+0x20/+0x24, error+0x5D and pointer validity+0x5E. WPADProbe802E159C takes channel/output type; game uses this result to clear a disconnected player's samples. Native player1 corresponds to the host mouse, other channels remain absent. Headless execution exposes no input. Guest buffers are validated before consuming edges or writing data.

Sample header cross-checked against [devkitPro KPAD documentation](https://wut.devkitpro.org/group__padscore__kpad.html); Wii132-byte total comes from this game's disassembly, rather than the later Wii U struct size. SDL's unscaled setting is documented in [SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE](https://wiki.libsdl.org/SDL3/SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE). No radio, IR-camera, Bluetooth or instruction interpreter introduced. Constant motion metadata is explicit TD-041 debt; additional host action bindings/controllers stay TD-013.

RED missing native API → GREEN test_input: raw movement and sign, invalid/nonfinite motion, short-click edges, focus reset, probe/disconnection, KPAD header, invalid guest buffer preserving pending input, headless service removal. Scoped Luna review reports no issues. Physical mouse/LMB acceptance and complete in-game save creation/loading need interactive confirmation; no automatic save-dialog bypass or simulated success added.

## Actual blend state

The existing Vulkan pipeline disabled blending, making transparent sprite rectangles opaque. Verified DOL functions80288274/802882C4/802882F0 update SDK shadow+0x220/+2. Native HLE preserves those writes and typed blend events in recorded resources; playback and queued draws own their blend state. The GPU uses state-keyed cached pipelines, with source/destination factors, subtract/logic operations and RGB/alpha write masks. Unsupported GPU logic operations fail explicitly. Full material parity remains TD-036.

RED missing DrawPacket blend state → GREEN GPU transparent/half-alpha fixtures. HLE fixture verifies SDK writes and recorded blend playback after the live state changes. Real20-second ordinary-validation run exits0 with1201retraces and zero MMIO/validation errors; readback shows actual check/cancel icons and the crosshair instead of white rectangles. This establishes these UI fixes, not full scene parity. Synchronization-validation/overlay debt TD-035 remains open.

## Russian text and application ID

Decoded font1 TGLP sheet from the dump contains substituted Cyrillic glyphs. English mes_en text therefore renders the wrong shapes. For the same save-dialog record, mes_us contains the Russian translation encoded for that modified font, while mes_ge retains German. The first German strings in mes_us are leftover untranslated entries; selecting language by a filename or the first strings alone was insufficient.

DOL80242984 calls SCGetLanguage, then OSGetAppGamename80259320 and compares four application-ID bytes to select regional English assets. OSGetAppGamename reads0x80003180, which previously remained zero although the disc header at0x80000000 correctly said RBUE08. Native init_lowmem now supplies the first8 application-ID/maker/version bytes from the actual header at0x80003180. This is immutable launch metadata, with translated SDK/game locale logic preserved.

RED low-memory assertion → GREEN. Real20-second trace now opens Data/message/mes_us.txtres and displays **«Сохранённые данные не найдены. Создать данные?»** with proper Cyrillic glyphs, crosshair and both button icons. Exit0,1127retraces,19opens/63reads/13669408bytes, zero unmappedMMIO/ordinary-validation errors. Files and language setting are preserved. Core input and renderer/save limitations stay in the debt register.

Final build.bat:55/55 CPU tests pass, dist staged/copied to <install dir>. Installed release launched from C:/Windows/Temp with --seconds12 --validation --skip-intro: exit0,659retraces,19opens/63reads/13669408bytes,131copies/presents, zero unmappedMMIO/ordinary-validation errors, all workers joined. No new commit requested. Full physical-click/save-format/movie acceptance remains pending user interaction; GPU screenshots alone do not prove that workflow.
