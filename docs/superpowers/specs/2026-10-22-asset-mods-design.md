# Asset mods (design)

Date: 2026-10-22. Status: draft for the owner's review. Follows the mod platform (`2026-10-22-mod-platform-design.md`), which already replaces whole disc files and textures. PBR materials were dropped by the owner and are not part of this.

## Intent

A mod author should be able to change what the game plays and says without tools the game does not ship with: put an ordinary `.ogg` / `.wav` where a music stream is, edit strings in a plain text file, replace a model inside an archive. Today a mod can replace a file only with a file in the game's own format at the same path, and cannot touch one item inside an archive. This design closes that gap for audio and text first (the port converts ordinary formats itself), then archive members, and treats video as research.

## What the game stores (RBUE08, `game/DATA/files/Data/`, 3.8 GB)

| Kind | Where | Format | Facts measured 2026-10-22 |
|---|---|---|---|
| Music, voices, cutscene audio | `sound/stream/*.brstm` (1334 files, 573 MB) | BRSTM (NW4R stream) | all DSP-ADPCM (codec 2); 1127 mono 32 kHz, 114 stereo 32 kHz without loop, 59 stereo 32 kHz looped, 34 at 48 kHz; largest 16 MB |
| Sound effects | `sound/biosound.brsar`, `bioUCprepro.brsar`, `SNDEFF.dat` | BRSAR sound archives | not parsed yet |
| Movies | `movie/*.sfd` (59) | CRI Sofdec; decoded by the statically translated game code | the native audio path mixes PCM8, PCM16 and DSP-ADPCM voices (docs/spikes/G13) |
| Text | `message/mes_<lang>.txtres` (8 languages, ~250 KB each), `movie_jimaku*.csvtbl` (subtitle tables) | `TRES` container: header (magic, version 0x00010000, record count 2634, table offset 0x10), a table of record offsets, 20-byte records (id, 0, count, pointer into a string pool) | string pool encoding not yet decoded |
| Models, scenes, effects | `chara/*.arc`, `bg/`, `event/`, `effect/`, `script/*.arc` | U8 archives holding BRRES (textures, models) and scripts | the texture extractor already reads U8 (nested too) |

## Goals

1. **Audio streams (milestone 1).** A mod puts `mods/<id>/audio/<stream name>.ogg` or `.wav` (for example `00_Title.ogg` for `sound/stream/00_Title.brstm`). The port converts it to a BRSTM that matches the original's channel count and sample rate behaviour and plays it in place of the original. Loop points come from the file: the WAV `smpl` chunk, the Vorbis comments `LOOPSTART` / `LOOPLENGTH`, or `loop` in the mod manifest; no loop info means no loop. A mod may also ship a ready `.brstm` under `files/sound/stream/` (already works).
2. **Text (milestone 2).** A mod ships `mods/<id>/text/<language>.json` (`{ "<record id>": "string", ... }`, only the strings it changes; `all.json` applies to every language). The port patches the parsed `TRES` records at load. A tool prints the original strings with ids (`tools/dump_text`) so authors can find what to change. Subtitle tables (`csvtbl`) are in scope as plain CSV overrides.
3. **Archive members (milestone 3).** Files under `mods/<id>/files/<disc path>.arc!/<path inside>` replace or add one member of that U8 archive; the port rebuilds the archive at load (cached) instead of requiring the whole archive. Models are replaced this way with BRRES files made with existing external tools; the port adds `tools/arc_unpack` / `arc_pack` (U8) for authors.
4. Everything stays live-toggleable like the rest of the platform: the converted result is cached under the mod's cache folder keyed by a hash of the source file and converter version, so switching a mod on does not re-convert, and the overlay picks it up the next time the game opens the file.
5. Conversion never blocks the game: it runs on a worker thread when the mod is enabled (or at install for big files); until it finishes the original plays and the mod list shows "converting".

## Non-goals

- Video (`.sfd`) replacement is research only (milestone 4 is a spike with a written finding, not a feature): the game decodes Sofdec itself in translated code, so a replacement needs either an encoder for that format or a native movie path.
- Sound effects inside BRSAR banks: not in this design (needs the BRSAR/RWAR parser; later spec if wanted).
- Streaming-quality extras: no re-encoding of the originals, no loudness normalization, no resampling choices exposed to authors (the port picks).
- Authoring GUIs, a mod store, signing.
- Native mods (DLL) get nothing new here.

## Decisions taken with the owner

- Ordinary formats are converted by the port for audio and text (option A); video and models use the game's formats plus tools (option B).
- Order: audio, text, then video and models.

## Constraints and facts for the builder

- Overlay mechanics, load order and live toggling exist (`modding/overlay.*`, `docs/modding/files-and-textures.md`); the converted files enter the overlay like any other file, so "later mod wins" and the conflict list keep working.
- Native audio mixes PCM16 voices, so a PCM16 BRSTM may play; whether the game's NW4R stream player accepts PCM16 streams (codec 1) is unverified. If it does not, the converter needs a DSP-ADPCM encoder (the coefficient search of Nintendo's dspadpcm is well documented). Decide with a spike before building the converter; PCM16 files are about 3.5 times larger than the originals.
- Stream playback reads the file through the DVD layer in blocks; a BRSTM written by the converter must have valid block sizes, loop fields and (for ADPCM) per-block history so the game's own parser accepts it.
- Ogg Vorbis decoding: use `stb_vorbis` (public domain, one file) unless the builder finds a reason not to. MP3 and FLAC are not required.
- Text records carry ids; mods address strings by id. The pool encoding and any control codes inside strings (colours, line breaks, button icons) must be found in a spike and passed through untouched in the JSON as escape tokens.
- All work happens on non-guest threads or at load; nothing may add waits to the guest thread.
- Tests: converter round trips (decode the produced BRSTM with an independent decoder and compare to the source), TRES parse/patch round trip on the real files of the owner's install, archive rebuild equals the original when no member changes, harness scenarios that play a replaced stream and show a changed string.

## Calls made while writing this (mine, not the owner's: check them)

- **Manifest keys.** One optional `audio` object in `mod.json`: `{ "audio": { "<stream name>": { "loop_start": <sample>, "loop_end": <sample> } } }`, samples of the mod's own file (the port rescales them to the converted rate). The manifest wins over loop info inside the file. Other new keys: none (`text/` and `audio/` folders need no declaration).
- **Rate and channels.** The converted BRSTM keeps the original's sample rate (32 or 48 kHz) and channel count: the port resamples (linear-phase windowed sinc, offline, so cost does not matter), downmixes stereo to mono and duplicates mono to stereo. The player's tolerance for other rates is not relied on.
- **PCM16 or ADPCM.** The plan carries both branches, decided by the first spike: PCM16 if the game accepts it (no cap on converted size, 16 MB originals become about 56 MB in the mod's cache folder), otherwise the DSP-ADPCM encoder joins milestone 1 with a round-trip test against an independent decoder.
- **Names.** `sound/stream/` is flat (1334 files, no subfolders, no duplicate names ignoring case), so `audio/<stem>.ogg|.wav` maps by stem, case-insensitive. Inside one mod a ready `.brstm` under `files/` wins over `audio/` with the same stem (the explicit game format); between mods the load order decides as always.
- **Archive members.** The game loads archives as whole files through the DVD layer, which already serves overlay files of any length; whether a bigger rebuilt archive is safe for the translated U8 loaders is checked by a spike in milestone 3 (replace one member by a bigger file and load the area). New members are added with rebuilt file and string tables; the cache key covers every enabled mod's members in load order.
- **Text.** The patch rewrites the `txtres` file (the pool is rebuilt at the new size) and serves it through the overlay, so no hook in guest memory is needed. Longer strings are accepted unless the spike finds a buffer limit in the game's text layout; then the port reports the limit per string. Encoding of the JSON is UTF-8; the pool encoding (found in the spike) decides what characters each language can hold.
- **csvtbl.** Whole-file replacement only (already works); per-row patching is not part of this design.
- **Status.** Conversion state is reported through the platform's existing per-mod status text (the same place that shows "enabled but not working"), no new channel. "Big file" threshold for converting at install: 4 MB of source audio.
- **Tests.** Converter, TRES and U8 tests use small synthetic fixtures and run everywhere; tests that need the owner's install are separate harness scenarios and skip when the game data is missing.

## Left to the builder

Container details of the converter, cache location and naming, how "converting" is shown in the Mods tab, error messages for bad input files (wrong sample rate, unsupported channel count, clipping), how channel counts that differ from the original are handled (the original is mono, the mod is stereo: downmix is the proposed default, a warning in the log).

## Order of work

1. Spike: PCM16 BRSTM accepted by the game? (replace one stream by a hand-made PCM16 BRSTM, play it through the harness).
2. Audio converter and overlay hook, with the live check and the mod manifest keys.
3. TRES spike and `tools/dump_text`, then the text patch.
4. U8 member overlay and `arc_unpack` / `arc_pack`.
5. Movie research note.
