# Native: gameplay entry, movies, sound,240 FPS ceiling

Owner scope: checkpoint commit first; fix crash at train/Rebecca/SMG confirmation, restore startup/title media, implement working sound, ceiling240FPS. Execute inline; preserve saves/files, no follow-up commit without request.

- [x] Checkpoint8598a85:55/55tests, clean tracked workspace before changes.
- [x] Capture actual crash using isolated copied saves and real player actions: missing entry80303630. Verify DOL boundary and add to ahead-of-time config.
- [x] Add240FPS presentation deadline, preserve original VI/game timing and copy-only progress. Test exact boundary and recovery after stall.
- [x] Rebuild and repeat actual mission confirmation; resolve verified further entries/boundaries until gameplay begins. No successful-load bypass.
  Owner confirmed gameplay begins after odd-stage TEV fix. HUD/crosshair/game events run;3Dscene remains black, so visual acceptance stays open.
- [ ] Import actual prebuilt NW4R mesh/setup assets at verified80366D3C/80366E14SDK boundaries. Raw resource submissions currently miss native geometry capture. Use typed geometry/array metadata ingestion; no generic FIFO/device command executor. Bounded mesh dump and immutable-copy regression prepared; real capture pending.
- [ ] Trace real SFD decode/draw path and title asset dependencies. Restore actual decoded picture/material state; reference screenshot does not establish the exact Wii scene. No unrelated replacement movie.
  Progress: opening and mission movies confirmed; title background appears after native TEV import, animation still unresolved.
- [ ] Verify native sound production boundary (AX voices/BRSTM/DSP ADPCM) and sample/control layout from DOL. Implement native PCM output using installed SDL3; preserve real start/stop/volume/rate events, bound queue and shutdown. No DSP microcode interpreter.
  Progress: native32000Hz AX/SDL output confirmed audible; button SFX and complete effects acceptance remain open (TD-042).
- [ ] CPU codec/boundary regressions, GPU validation/readback, actual media/audio playback and mission transition. Full build/dist/install; record remaining fidelity/performance limits in tech debt.

Critical limits: current soundtrack archives use BRSTM/BRSAR, movies SFD/MPEG program stream. Startup-logo skipping is independent of missing movies. Ceiling240 does not synthesize240unique frames or accelerate60Hzlogic. Audio callbacks must not read mutable guest memory on a host device thread; publish owned PCM/control under the guest baton.
