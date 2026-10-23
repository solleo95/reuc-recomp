# Asset mods, movies: `sfd_tool` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (inline, as for the earlier milestones). Steps use checkbox syntax.

**Goal:** An author runs `tools\sfd_tool.bat make my-video.mp4 MV_000_0.sfd` and gets a movie file the game plays in place of the original (put under `mods/<id>/files/Data/movie/`).

**Architecture:** `tools/sfd_tool/sfd_tool.py` (Python 3, no packages) asks the author's ffmpeg for MPEG-1 video (640x480, 29.97 fps, no B-frames) and ADX audio (stereo, 32 kHz), then interleaves them into a Sofdec file itself: 2048-byte sectors, each a pack header plus one packet, video packets with a PTS on the first picture that starts in them, audio packets with a PTS per sector. `info` prints what a `.sfd` holds (works on the game's own movies). The port does not change: a replacement movie is a whole-file replacement, which already works.

**Spec:** `docs/spikes/M7-video.md` (measured layout and the live tests) and `docs/superpowers/specs/2026-10-22-asset-mods-design.md` (video was research only; this makes it a tool, the owner asked for it).

## Layout facts the tool relies on (from M7, checked on the owner's install)

- Sector = `00 00 01 BA` + 8 bytes (SCR, mux rate `88 24 29`) + packets, 2048 bytes exactly. SCR of sector i = `i * 14` ticks (the mux rate ramp).
- Sector 0 (and 1): system header `00 00 01 BB 00 09 88 24 29 06 20 FF C0 C0 04`, then a `BE` padding packet of length 2015 (`0F` then `FF` bytes).
- Video sector: `00 00 01 E0`, length 2030, then `21 <PTS>` (5 bytes, PTS only when a picture starts in the sector's data) or `0F`, then 2025 or 2029 bytes of the MPEG-1 elementary stream. A final `00 00 01 B7` ends the stream, zero bytes pad the last sector. PTS = picture index x 3003 (no B-frames, so PTS = DTS).
- Audio sector: `00 00 01 C0`, length 2023, `40 04` (STD buffer), 5-byte PTS = n x 5040 (n = audio sector number; 2016 ADX bytes are 0.056 s at 32 kHz stereo), 2016 ADX bytes (the ADX file whole, header included, split in order; zero pad at the end is silence), then the padding packet `00 00 01 BE 00 01 0F`.
- The game plays a file until it runs out; the file therefore ends right after the last sector that carries data. The CRI packets of the originals (`0xBF`: "SofdecStream" and CRITAGS with frame counts) are the open question: the first live check runs without them; if the game needs them the plan adds them (Task 3, step 4).
- ffmpeg cannot write this container, but it reads it, so tests can verify the output with `ffmpeg -i` (video `mpeg1video` 640x480 29.97, audio `adpcm_adx` 32000 Hz stereo).

## Global Constraints

- Pure standard library Python 3.10+; ffmpeg only as an external program found on `PATH` or given with `--ffmpeg`; nothing is installed or downloaded by the tool.
- Output is exactly what ffmpeg produced for the picture (no re-compression by the tool); the video bitrate default is 2500 kb/s with `-maxrate` the same and `-bufsize 655360`; `--bitrate` overrides it.
- Never overwrite an existing output file without `--force`; never write next to the game's files unasked; temp files live in a temp folder and are deleted.
- Errors go to stderr with a non-zero exit code and one clear sentence (no traceback for ordinary mistakes).
- Edit source only with Edit/Write; tests: `ctest --test-dir build --output-on-failure -R sfd_tool`; the live check is `python -m uctest run movie_replace`; full `build.bat` deploy at the end.

## Review Focus

1. An input with no audio track, a longer audio than video, a very short clip (a few frames), a very long one (a bitrate that does not fit the 4 GiB or the 2^33 PTS range is not an issue, but an hour of video is: refuse above 30 minutes).
2. Odd input: other sizes and aspect ratios (`--fit pad|stretch|crop`), variable frame rate sources, non-ASCII paths, a path with spaces.
3. A damaged or foreign `.sfd` given to `info` (not a pack stream, truncated sector, packet length past the sector): a clear error, never an exception trace.
4. ffmpeg missing, or built without `mpeg1video` / `adpcm_adx`: say so and which encoder.
5. Output sector accounting: the file length is a multiple of 2048; every packet fits its sector; no sector is empty.

---

### Task 1: sfd writer and reader (`mux`, `parse`) with synthetic tests

**Files:** Create `tools/sfd_tool/sfd_tool.py` (functions only, no ffmpeg yet), `tests/test_sfd_tool.py`

**Interfaces:**
- Produces:
  - `def mux(es: bytes, adx: bytes) -> bytes` : the .sfd bytes for an MPEG-1 elementary stream (it appends the sequence end code itself) and an ADX file; interleaves by time (video earlier than audio by 0.5 s of lead: the next sector goes to the stream whose next data has the lower time, time of video data = PTS of its next picture start minus 0.5 s, time of audio = n x 5040 ticks).
  - `def parse(data: bytes) -> SfdInfo` with `SfdInfo(sectors: int, video_bytes: bytes, audio_bytes: bytes, pictures: int, picture_pts: list[int], audio_pts: list[int], scr: list[int], problems: list[str])` : reads the 2048-byte sector structure, concatenates the video and audio payloads, collects PTS; raises `SfdError` (a `ValueError`) for a file that is not a pack stream of whole sectors.
  - `class SfdError(ValueError)`.

- [ ] Tests first (`tests/test_sfd_tool.py`, synthetic inputs: an ES of a sequence header `00 00 01 B3` + 40 pictures `00 00 01 00` + 8 pseudo-random bytes of varied length (a few of 3000 bytes so some pictures span sectors, many of 40 bytes so several start in one sector), an ADX file made of the header `80 00 00 20 03 12 04 02 ...` (0x24 bytes) and 36-byte frames): `mux_output_is_whole_sectors` (length multiple of 2048, every sector starts with the pack header, SCR strictly increasing), `video_payload_round_trips` (`parse(mux(es, adx)).video_bytes` starts with `es` and the rest is the sequence end code and zeros), `audio_payload_round_trips` (starts with `adx`, rest zeros, a multiple of 2016), `pts_rules` (the first picture has PTS 0; picture k's PTS, when present, is 3003 x k; audio sector n has PTS 5040 x n; a sector in which no picture starts has none), `streams_are_interleaved` (no stretch of more than 40 sectors of one stream when both have data), `header_sectors_are_first` (sector 0 holds the system header), `long_audio_and_short_video_and_the_reverse` (the audio runs on after the video ends and the reverse; the file ends after the last data sector), `parse_rejects_foreign_files` (empty, 10 bytes, random bytes, a sector with no pack header, a packet longer than its sector, a file of 2047 bytes) with `SfdError`. Run: FAIL (module missing).
- [ ] Implement `mux`, `parse`, `SfdError` as specified; run the tests: PASS. Commit "Add sfd_tool: Sofdec file writer and reader".

### Task 2: `make` and `info` commands (ffmpeg)

**Files:** Modify `tools/sfd_tool/sfd_tool.py` (command line), Create `tools/sfd_tool/sfd_tool.bat`, `tools/sfd_tool/CMakeLists.txt` (install + the two tests), Modify `CMakeLists.txt` (add_subdirectory), `tests/test_sfd_tool.py`

**Interfaces:**
- Consumes: `mux`, `parse`, `SfdError` (Task 1).
- Produces: `python sfd_tool.py make <input> <output.sfd> [--ffmpeg PATH] [--bitrate KBPS] [--fit pad|stretch|crop] [--force]`, `python sfd_tool.py info <file.sfd>` (prints size, sectors, picture count, duration, video and audio bytes, problems; exit 1 for a file `parse` refuses). Functions `find_ffmpeg(explicit: str | None) -> str` (raises `ToolError` with the missing program or encoder), `encode_video(ffmpeg, src, fit, bitrate, workdir) -> Path` (an MPEG-1 elementary stream), `encode_audio(ffmpeg, src, seconds, workdir) -> Path` (an ADX file of exactly that many seconds, silence when the source has no audio or too little), `class ToolError(Exception)`.

- [ ] Tests first, with ffmpeg (skipped with a printed reason when none is found): `make_from_a_generated_clip` (lavfi `testsrc2` 1 s at 320x240 and a sine: `make` succeeds; `ffmpeg -i out.sfd` lists `mpeg1video`, `640x480`, `29.97 fps`, `adpcm_adx`, `32000 Hz`, `stereo`; duration 1.0 s within 0.1; `info` agrees on the picture count 30 or 29 +- 1), `source_without_audio_gets_silence`, `fit_modes_give_640x480` (`pad`, `stretch`, `crop` from a 640x360 source), `existing_output_needs_force`, `missing_input_and_bad_arguments_are_clear_errors` (non-zero exit, a one-line stderr, no `Traceback`), `no_ffmpeg_is_explained` (`--ffmpeg` pointing to a missing file), `refuses_over_30_minutes` (a lavfi source declared 31 minutes is refused before encoding by probing the duration), `info_on_a_foreign_file` (exit 1, no trace), `non_ascii_and_spaced_paths`. Run: FAIL.
- [ ] Implement; run: PASS. Commit "Add sfd_tool make and info".

### Task 3: live check, then docs and deploy

**Files:** Modify `scripts/uctest/scenarios.py` (scenario `movie_replace` with a prepare step, like `arc_member`), `.gitignore` (the generated mod), Create `docs/modding/video.md`, Modify `docs/modding/guide/en/files.md`, `docs/modding/guide/ru/files.md`, `docs/modding/guide/en/README.md` / `ru/README.md` only if a task table lists movies, `docs/spikes/M7-video.md` (a line pointing to the tool)

- [ ] Failing check first: scenario `movie_replace` builds, with `sfd_tool make`, a 9 s clip (a solid red picture for 3 s, a solid green one for 3 s, a solid blue one for 3 s; a 440 Hz tone) from ffmpeg `lavfi` sources, installs it as the test-only mod `movie_replace` (`files/Data/movie/MV_000_0.sfd`), starts the game without skipping the opening movie, captures a frame every second for 16 s of play and requires: some capture whose centre colour is red-dominant, a later one green-dominant, a later one blue-dominant, and a final capture that is none of the three (the title screen, the movie ended and the game went on); the exit log must show `audio stats: ... nonzero samples` above 100000. Skipped with a reason when ffmpeg is missing. Expected before the tool exists: the scenario's prepare step fails (no `sfd_tool`).
- [ ] Run it with the tool built in Tasks 1 and 2: PASS. If the movie never ends or the picture is wrong, add the CRI packets (`0xBF` "SofdecStream" and CRITAGS, the layout is in `docs/spikes/M7-video.md`, the frame counts patched) to `mux` with a test, and run again; record the result in the ledger.
- [ ] Docs: `docs/modding/video.md` (what a movie file is, how to make one with `sfd_tool`, which movie plays where (the file names are listed by `--log-dvd`), limits: 640x480 29.97 only, an hour is refused, no B-frames, the ffmpeg requirement, rights), a "Movies" section in both guides' `files.md`, a line in the M7 note. The docs link test passes.
- [ ] Full `ctest`, full `build.bat` deploy, one Opus review of the whole change, fix findings (each with a failing test first), commit "Movie replacement tool (sfd_tool)".
