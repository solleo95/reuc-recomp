# Asset mods, milestone 1: audio streams Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A mod puts `audio/<stream stem>.ogg|.wav` in its folder and the game plays it in place of `sound/stream/<stem>.brstm`, converted by the port to a PCM16 BRSTM.

**Architecture:** A converter library in `modding/` (WAV and Ogg readers, resampler and channel mapper, BRSTM PCM16 writer) with no game dependencies, and an `AudioConverter` that runs it on a worker thread per active mod, caches results under `mods/.cache/<id>/` and exposes a generated overlay root that `GameMods::refresh_files` puts before the mod's own `files/` root. Nothing touches the guest thread.

**Tech Stack:** C++20, `stb_vorbis` (vendored, public domain), the existing mod platform (`modding/`, `game/game_mods.*`, `runtime/dvdfs.h`), CTest, the `uctest` harness.

**Spec:** `docs/superpowers/specs/2026-10-22-asset-mods-design.md` (this plan is milestone 1 only; text, archive members and the movie note get their own plans after their spikes).

## Spike result (2026-10-22, done)

A PCM16 BRSTM (codec 1) plays in the game: `tools/spikes/brstm_pcm16.py` rewrote `00_Title.brstm` (stereo, 32 kHz, looped, 8.3 MB) as PCM16 (28.9 MB, `ADPC` chunk zero-filled, `HEAD` kept, block size 0x2000, 4096 samples per block). Title screen for 900 ticks: the original gives 1 004 807 nonzero samples in the native audio stats, the PCM16 file 1 014 023, and the same file silenced gives 0. So no DSP-ADPCM encoder is needed. Loop playback past the loop point and mono streams are not yet checked (Task 7 checks them).

BRSTM layout used (all big-endian): `RSTM` header 0x40 bytes (BOM FEFF, version 0x0100, file size, header size 0x40, chunk count, then offset/size of HEAD, ADPC, DATA); `HEAD` chunk with three (marker, offset) entries at +8 pointing to stream info, track info and channel info; stream info at HEAD+8+offset: codec, loop flag, channels, sample rate (u16 at +4), loop start (+8), total samples (+0xC), audio data start (+0x10, absolute), block count (+0x14), block size (+0x18), samples per block (+0x1C), last block size (+0x20), last block samples (+0x24), last block padded size (+0x28); audio data is block-interleaved per channel.

## Global Constraints

- Converted BRSTM keeps the original stream's sample rate (32 000 or 48 000 Hz) and channel count (1 or 2); stereo to mono is a downmix, mono to stereo duplicates.
- Manifest key: one optional `"audio"` object in `mod.json`: `{ "<stream stem>": { "loop_start": <sample>, "loop_end": <sample> } }`, samples of the mod's own file, rescaled to the converted rate; the manifest wins over loop info in the file; no loop info means no loop.
- `audio/<stem>.ogg|.wav` maps to `sound/stream/<stem>.brstm` by stem, case-insensitive; inside one mod a ready `.brstm` under `files/` wins over `audio/` with the same stem; between mods load order decides.
- Conversion runs on one worker thread, started at game start for enabled mods and when a mod is switched on (the spec's separate "at install for big files" case is dropped: one path, cached); until it finishes the original plays; state shows in the mod's existing status text ("converting audio", errors).
- Cache key: hash of source bytes, converter version, target rate, target channels and the manifest loop values; location `mods/.cache/<id>/audio/<hash>.brstm`; the generated overlay root is `mods/.cache/<id>/generated/` with the disc layout (`Data/sound/stream/<stem>.brstm`).
- Never add waits to the guest thread; the converter uses no game state.
- Tests use small synthetic fixtures and run everywhere; tests needing the owner's install are harness scenarios that skip when the game data is missing.
- Source limit: 30 minutes of audio per file (error beyond).
- Edit source only with Edit/Write; tests: `ctest --test-dir build --output-on-failure -R <name>`; full build and deploy at the end of the milestone (`build.bat` from PowerShell).

## Review Focus

1. A mono source for a stereo original (and the reverse): downmix/duplicate, no crash, a log line.
2. A 44.1 kHz or 48 kHz source for a 32 kHz original: pitch and duration stay right (a 1 s tone stays 1 s long).
3. Loop points beyond the file or reversed (`loop_start >= loop_end`): the loop is dropped with a log line, the stream still plays.
4. 24-bit and float WAV, WAV with extra chunks before `data`, truncated and zero-length files: error message names the file, nothing else is affected.
5. A mod `audio/foo.ogg` with no `sound/stream/foo.brstm` on the disc: ignored with an error line (the game cannot name a new stream).
6. The mod is switched off or the game exits mid-conversion: the worker stops, no half-written cache file is ever used (write to a temp name, rename).
7. Clipping and silence: samples clamp to int16; an all-zero file converts and plays.

---

### Task 1: BRSTM PCM16 writer and reader

**Files:**
- Create: `modding/audio/brstm.h`, `modding/audio/brstm.cpp`
- Test: `tests/test_brstm.cpp` (+ `modding/CMakeLists.txt`, `tests/CMakeLists.txt` entries)

**Interfaces:**
- Produces:
  - `struct PcmAudio { uint32_t rate = 0; std::vector<std::vector<int16_t>> channels; std::optional<std::pair<uint32_t,uint32_t>> loop; };` (loop = start sample, end sample exclusive; a BRSTM loop runs to the end of the stream, so the writer requires `end == frames` after Task 4 trims; see Task 4)
  - `std::vector<uint8_t> write_brstm_pcm16(const PcmAudio& audio);` throws `std::invalid_argument` for 0 or more than 2 channels, empty audio, channel length mismatch
  - `std::optional<PcmAudio> read_brstm(std::span<const uint8_t> file);` decodes codec 1 (PCM16) and codec 2 (DSP-ADPCM, coefficients from the channel info) so tests can compare against the game's own files; nullopt for anything else
  - `struct BrstmHeader { uint8_t codec, channels; uint32_t rate, frames; bool looped; uint32_t loop_start; }; std::optional<BrstmHeader> read_brstm_header(std::span<const uint8_t> first_bytes);` (needs the first 0x100 bytes; used by Task 5)

- [ ] **Step 1: Write the failing tests** in `tests/test_brstm.cpp`: `pcm16_round_trip_mono` (1000-sample ramp, 32000 Hz, no loop: `read_brstm(write_brstm_pcm16(a))` equals `a`), `pcm16_round_trip_stereo_with_loop` (3 blocks plus a partial block, loop 5000..end: samples, rate, loop equal; header says looped, loop_start 5000), `header_fields` (block size 0x2000, samples per block 4096, data start equals DATA chunk offset + 0x20, ADPC chunk present and zero-filled, file size field equals the byte count), `rejects_bad_input` (0 channels, 3 channels, empty, ragged channels throw), `adpcm_decode_matches_reference` (a 3-frame DSP-ADPCM file built in the test with known coefficients decodes to the values computed by the standard formula `(nibble*scale<<11 + 1024 + c1*y1 + c2*y2) >> 11`, clamped).
- [ ] **Step 2: Run to verify it fails** (`ctest -R test_brstm`): FAIL, unresolved symbols.
- [ ] **Step 3: Implement** `brstm.cpp` following the layout in "Spike result"; port `tools/spikes/brstm_pcm16.py` (parse, decode_adpcm, write_pcm16) to C++. The track-info and channel-info tables of the HEAD chunk are written like the original's (one track, one channel info entry per channel pointing at a zeroed 0x30-byte ADPCM parameter block); copy the exact byte layout from `tools/spikes/brstm_pcm16.py` and a real file (`sine.brstm` in the game's `Data/sound/stream/` is 352 bytes and shows it for mono).
- [ ] **Step 4: Run tests**: PASS.
- [ ] **Step 5: Cross-check against the game's own file** (manual, not a test): run `read_brstm` on `VO_2201003.brstm` and compare with the Python decode (first 8 samples `44, -132, -311, -316, -180, -188, -224, -288` from index 1000 in the spike run). Then commit: `git add modding/audio tests/test_brstm.cpp modding/CMakeLists.txt tests/CMakeLists.txt tools/spikes/brstm_pcm16.py && git commit -m "Add BRSTM PCM16 writer and reader"`.

### Task 2: WAV reader

**Files:** Create `modding/audio/wav.h`, `modding/audio/wav.cpp`; Test `tests/test_audio_decode.cpp`

**Interfaces:**
- Consumes: `PcmAudio` (Task 1; here the loop is in samples of the source)
- Produces: `struct DecodeResult { std::optional<PcmAudio> audio; std::string error; };` and `DecodeResult read_wav(std::span<const uint8_t> file);` supports PCM 8/16/24/32-bit integer and 32-bit float, 1 to 2 channels (more: error), chunks in any order (skips unknown ones, `data` may follow `LIST`), odd-size chunk padding, and the `smpl` chunk's first loop (start, end+1 as exclusive end) as `loop`.

- [ ] **Step 1: Failing tests** (`wav_pcm16_mono`, `wav_24bit_and_float_scale_to_int16`, `wav_skips_unknown_chunks_and_pads_odd_sizes`, `wav_smpl_loop_is_read`, `wav_errors_name_the_problem`: truncated header, `data` longer than the file, 0 channels, 3 channels, unsupported format tag, zero frames) with assertions on exact sample values.
- [ ] **Step 2: Run, expect FAIL. Step 3: Implement** `read_wav`; floats clamp to [-1, 1] then scale by 32767, 24-bit shifts down with rounding. **Step 4: PASS. Step 5: Commit** "Add WAV reader".

### Task 3: Ogg Vorbis reader

**Files:** Create `modding/third_party/stb_vorbis.c` (vendored, version noted in a header comment; `#define STB_VORBIS_HEADER_ONLY` not used), `modding/audio/ogg.h`, `modding/audio/ogg.cpp`; Test: extend `tests/test_audio_decode.cpp`

**Interfaces:**
- Produces: `DecodeResult read_ogg(std::span<const uint8_t> file);` uses `stb_vorbis_open_memory` and `stb_vorbis_get_samples_short_interleaved`; loop from the Vorbis comments `LOOPSTART` and `LOOPLENGTH` (end = start + length) or `LOOPEND`; more than 2 channels is an error; stb's error code is turned into a message.

- [ ] **Step 1: Failing tests** with a tiny Ogg fixture embedded as a byte array in the test (a 0.1 s mono 32 kHz tone encoded once with `oggenc` or ffmpeg and pasted; keep it under 4 KB): `ogg_decodes_tone_length_and_rate`, `ogg_garbage_is_an_error`, `ogg_loop_comments` (a second fixture with `LOOPSTART=100`, `LOOPLENGTH=500`).
- [ ] **Step 2-4: Run FAIL, vendor stb_vorbis (download the single file from the stb repository, record its version), implement, PASS.** Add the file to `uc_mods` with warnings off for it. **Step 5: Commit** "Add Ogg Vorbis reader".

### Task 4: Resample, channel mapping and loop fitting

**Files:** Create `modding/audio/convert.h`, `modding/audio/convert.cpp`; Test `tests/test_audio_convert.cpp`

**Interfaces:**
- Consumes: `PcmAudio`
- Produces: `struct ConvertOptions { uint32_t target_rate; uint32_t target_channels; std::optional<std::pair<uint32_t,uint32_t>> loop_override; };` `struct ConvertResult { std::optional<PcmAudio> audio; std::vector<std::string> notes; std::string error; };` `ConvertResult convert_audio(PcmAudio source, const ConvertOptions& options);`
  - resamples with a windowed-sinc filter (32 taps per side, Kaiser or Blackman window) when rates differ; output length is `round(frames * target / source)`
  - channels: 2 to 1 averages, 1 to 2 duplicates; a `notes` entry says which happened
  - loop: `loop_override` if set, else the source's loop; both scaled by the rate ratio; the loop end is then trimmed: audio is cut to the loop end (the BRSTM loop runs to the stream's end) and the result's `loop` is `(start, frames)`; invalid loops (`start >= end`, `end > frames`) are dropped with a note
  - error for empty audio or a source over 30 minutes

- [ ] **Step 1: Failing tests:** `same_rate_same_channels_is_identity`, `rate_conversion_keeps_duration_and_frequency` (1 s 440 Hz sine at 44100 to 32000 gives 32000 frames and a zero-crossing count within 1 of the original's, and RMS within 2%), `stereo_to_mono_averages`, `mono_to_stereo_duplicates`, `loop_is_rescaled_and_audio_trimmed` (loop 44100..88200 at 44.1 kHz to 32 kHz gives loop 32000..64000 and 64000 frames), `bad_loops_are_dropped_with_a_note`, `empty_and_overlong_are_errors`, `clipping_is_clamped`.
- [ ] **Step 2-4: FAIL, implement, PASS. Step 5: Commit** "Add audio resampling and channel mapping".

### Task 5: Manifest `audio` key

**Files:** Modify `modding/manifest.h` (`std::map<std::string, AudioLoop> audio;` with `struct AudioLoop { uint32_t loop_start, loop_end; };` in `ModManifest`), `modding/manifest.cpp`; Test: `tests/test_mod_manifest.cpp` (existing manifest tests; find the file with `grep -l parse_manifest tests/*.cpp`)

**Interfaces:** Produces `ModManifest::audio` keyed by lower-case stream stem. Parse rules: object of objects; keys `loop_start` and `loop_end` whole numbers, `0 <= start < end <= 2^31`; stems are `[A-Za-z0-9_-]+`; any violation is a manifest error naming the key.

- [ ] **Step 1: Failing tests** `audio_loops_parse`, `audio_loop_start_not_before_end_is_an_error`, `audio_stem_with_a_path_is_an_error`, `audio_key_is_optional`. **Step 2-4** FAIL, implement, PASS. Also update `docs/modding/manifest.md` (the `audio` field). **Step 5: Commit** "Add the audio key to mod.json".

### Task 6: AudioConverter (worker, cache, generated overlay)

**Files:** Create `modding/audio/audio_converter.h`, `modding/audio/audio_converter.cpp`; Test `tests/test_audio_converter.cpp`

**Interfaces:**
- Consumes: `read_wav`, `read_ogg`, `convert_audio`, `write_brstm_pcm16`, `read_brstm_header`, `ModEntry`, `ModManifest::audio`
- Produces:
  ```cpp
  struct StreamOriginal { uint32_t rate; uint32_t channels; };   // from the disc's BRSTM header
  using OriginalLookup = std::function<std::optional<StreamOriginal>(const std::string& stem)>;   // nullopt: no such stream on the disc
  class AudioConverter {
  public:
      AudioConverter(std::filesystem::path cache_root, OriginalLookup lookup, std::function<void()> on_change);
      ~AudioConverter();   // stops the worker, joins
      void set_active(const std::vector<const ModEntry*>& mods);   // enables conversion for these mods (cancels work of mods no longer in the list)
      std::filesystem::path generated_root(const std::string& mod_id) const;   // <cache_root>/<id>/generated; created on demand
      std::string status(const std::string& mod_id) const;   // "" when idle and fine; "converting audio (2 of 5)"; "audio: <file>: <error>" lines joined by "; "
  };
  ```
  `on_change` is called from the worker after each file finished (the caller rebuilds the overlay). A finished file is written to `<cache_root>/<id>/audio/<hash>.brstm` through a temp name and renamed, then copied (hard link if possible) to `generated/Data/sound/stream/<stem>.brstm`; stale generated files of the mod (sources removed or changed) are deleted at `set_active`. Cache hits skip conversion.

- [ ] **Step 1: Failing tests** with a temp folder, a synthetic WAV written by the test and a fake `OriginalLookup`: `converts_and_publishes_generated_file` (wait on a condition variable fed by `on_change`; the generated file exists, `read_brstm` gives the expected rate/channels/length), `cache_hit_does_not_reconvert` (second `AudioConverter` over the same cache calls `on_change` without re-reading: check by deleting the source after the first run and keeping the cache key via a sidecar `<hash>.src` stamp, or by counting calls to a test hook), `unknown_stream_is_reported_and_ignored`, `bad_file_reports_error_and_others_still_convert`, `switching_a_mod_off_removes_its_generated_files`, `destructor_stops_a_long_conversion` (a 20-minute silent source: destruction returns within 2 s), `manifest_loop_wins_over_file_loop`, `half_written_temp_files_are_never_published`.
- [ ] **Step 2-4: FAIL, implement (one `std::thread`, a mutex-guarded job queue, an atomic stop flag checked per block of the resampler through a callback or per 64k frames), PASS. Step 5: Commit** "Add the audio converter".

### Task 7: GameMods integration, harness check, docs

**Files:** Modify `game/game_mods.h`, `game/game_mods.cpp` (own an `AudioConverter`; `refresh_files` builds `files_roots` as, per active mod, `[generated_root(id) if it exists, mod/files if it exists]`; `OriginalLookup` reads the first 0x100 bytes of `Data/sound/stream/<stem>.brstm` through `DvdFs` with the overlay of earlier mods ignored: use the disc entry as the base; on `on_change` call `refresh_files(false)` on the next `apply()` or from a posted callback, whichever the existing menu thread rule allows; mod status text appends `AudioConverter::status`), `scripts/uctest/scenarios.py` (scenario `audio_replace`), `scripts/uctest/mods/audio_tone/` (test-only mod: `mod.json` plus a WAV tone generated by the scenario into the installed copy at run time, never committed), `docs/modding/audio.md` (new) and `docs/modding/files-and-textures.md` (link), `docs/modding/manifest.md`.

**Interfaces:** Consumes `AudioConverter` (Task 6). Produces: scenario `audio_replace`.

- [ ] **Step 1: Failing check first**: write the scenario `audio_replace`. It generates a 3 s, 440 Hz, mono, 44.1 kHz WAV with a `smpl` loop 0..1 s as `audio/00_Title.wav` into the installed test mod, and spends 15 s of game time at the title (speed 8). Evidence: `audio stats: ... nonzero samples N` in run.log. A control scenario `audio_replace_silent` uses an all-zero WAV and must give N = 0 (proves the title audio is the replaced stream); the tone run must give N > 0 and different from the plain title run's N (the original music stays out). A second step runs 60 s (past the 1 s loop point) and requires N to keep growing between the 30 s and 60 s readings (the test server reports audio stats via the exit log, so run two lengths and compare). Skips with a clear message when the game data is missing. Run it before implementing: expect FAIL (the original music plays, the silent control gives N > 0).
- [ ] **Step 2: Implement the GameMods integration**; conversion state shows in the Mods tab text through the existing per-mod messages.
- [ ] **Step 3: Run** `cd scripts && python -m uctest run audio_replace --speed 8` and the silence control: PASS.
- [ ] **Step 4: Mono original check**: a second scenario step replaces one mono voice line (`VO_2201003`, 5216 bytes, found in the spike) the same way and requires the game to keep running 10 s after it plays; record the result in `docs/spikes/M4-audio-conversion.md` together with the file sizes.
- [ ] **Step 5: Docs** (`docs/modding/audio.md`: folder layout, formats, loop points, manifest key, status messages, size note: converted files are about 3.5 times the original). **Step 6: full test run** (`ctest --test-dir build --output-on-failure`), **full `build.bat` deploy** per the owner's rule, commit "Audio replacement for mods (milestone 1)".

## Self-review

- Spec coverage: audio goals 1, 4 and 5 (Tasks 1-7), manifest keys (Task 5), rate/channel policy (Task 4), names and precedence (Task 7 root order, Task 6 lookup), status (Task 6, 7), tests policy (all tasks). Spec items for text, archive members and video belong to later plans.
- Types: `PcmAudio` (Task 1) is the type of `DecodeResult::audio`, `ConvertResult::audio` and the writer's input; `StreamOriginal` (Task 6) is built from `BrstmHeader` (Task 1).
- Not decided here on purpose: thread-safety of `on_change` versus the menu thread (the builder follows how `GameMods::apply` is called today), resampler tap count tuning.
