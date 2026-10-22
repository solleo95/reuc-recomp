# M4: audio conversion for mods (milestone 1 of the asset mods design)

Checked 2026-10-22 (USA build, harness at 1280x720).

- PCM16 BRSTM plays: see the spike result in `docs/superpowers/specs/2026-10-22-asset-mods-design.md`.
- Live checks with `uctest run audio_replace` and `audio_replace_silent` (mods `audio_tone` and `audio_silent`, a 1 s 22.05 kHz mono WAV with a loop over its first half, replacing `00_Title`): the silent file gives 0 nonzero samples in the exit statistics (the original music gives about 1 000 000 in 900 title ticks), the tone gives 965 642 in the same time, far more than the 0.5 s loop alone would, so the converted stream plays and loops. The mono 22.05 kHz source became the original's stereo 32 kHz stream.
- Not checked in the game: a mono original (the voice streams). The mono output path is unit tested (`test_game_mods`, `test_audio_converter`, `test_brstm`) and its header layout matches the game's own mono file `VO_2201003.brstm` byte for byte in size, but no voice line was played with a replacement. First thing to try if a voice replacement is silent.
- Conversion speed: a 44.1 kHz source resamples at well over real time (the 0.5 s tests take milliseconds; a 25-minute 8 kHz file is cancelled mid-way in the tests).
- Disk: the converted file is stored in `mods/.cache/<id>/audio/` and copied into `mods/.cache/<id>/generated/`, so a mod's audio costs twice the PCM16 size on disk. Hard links would halve it.
