# M7: replacing the game's movies (milestone 4 of the asset mods design: research)

Checked 2026-10-23 (USA build, harness at 1280x720, `uctest run movie_probe`; the test mod `movie_probe`, git-ignored, holds one hand-made `files/Data/movie/MV_000_0.sfd`, the opening movie). Tools: `tools/spikes/sfd_mux.py` (writes an `.sfd` from a new video and audio, using an original movie as the template).

**Result: yes, a movie can be replaced by a mod; the game's own (translated) Sofdec decoder plays a video and an audio track that were made with ffmpeg.** No decoder, native movie path or hook is needed. What is missing for a feature is a muxer for the game's container (ffmpeg cannot write it) and an ffmpeg on the author's computer.

## What the movies are

- 59 files, 2.4 GB (3.7 MB to 180 MB). All have the same picture: MPEG-1 video, 640x480, 29.97 fps, sequence-header bitrate 104857200 (a placeholder), with B-frames (GOP pattern IPB) and a PTS on the first picture of each packet.
- The container is an MPEG-1 system stream in **2048-byte sectors**: one pack header (12 bytes) and one packet per sector. Stream `0xE0` is the video, `0xC0` is the audio (**ADX**, stereo, 32 kHz, 18-byte frames, 288 kb/s, header 0x120 bytes: `80 00 01 1c 03 12 04 02 00 00 7d 00 ...`; ffmpeg reads it as `adpcm_adx`), `0xBE` is padding, `0xBF` carries the CRI packets.
- The first four sectors are the system header, padding and two `0xBF` packets: `SofdecStream`, the file name, a date and `Sofdec CRAFT/GUI Ver.1.82` with numbers (frame count 916 as a little-endian word), then a `CRITAGS` text with the picture size, frame rate, frame count (916 and 915 as text) and the encoder name.
- Pack timing is a plain ramp (about 14 ticks per sector, the mux rate), so the player is paced by the PTS values and the data it has read, not by the pack clock. Audio sectors each carry their own PTS (the template's can be kept when the new ADX has the same format).
- The game reads the file in whole through the DVD layer in chunks and plays until **the file runs out**.

## What was tested (all in the game)

1. **New video, original audio.** A 640x480 test pattern with a frame counter (ffmpeg `mpeg1video`, no B-frames, 1.4 Mb/s, 916 frames) put into the template's video sectors (a PTS on each sector where a picture starts; zero bytes after the sequence end code). It plays: the capture shows the counter at the right time (7.5 s at frame 225), the game's subtitles are drawn over it, and the game goes on after the movie.
2. **New audio.** An ffmpeg-made ADX (stereo, 32 kHz) put into the template's audio sectors (the template's PTS kept): a silent track gives `audio stats: nonzero samples 0`, a 660 Hz tone gives 998 624. The new ADX has ffmpeg's own header (0x24 bytes), which the game accepts.
3. **A shorter movie.** 300 frames and 10 s of audio in the 916-frame template: the picture stays on its last frame until the template's sectors run out (30 s), the game does not stop at the end of the picture stream. With the file **cut after the last sector that carries data** (2.6 MB) and the frame counts in the CRI packets changed to 300 (same width, so no byte moves), the movie ends after 10 s and the game goes to the title screen. Whether the tag change was needed was not tested.

**Done afterwards:** `tools/sfd_tool` (`make`, `info`; plan `docs/superpowers/plans/2026-10-23-asset-mods-video-tool.md`, user text `docs/modding/video.md`) writes the file layout itself and passed a live check (`uctest run movie_replace`: a red, green, blue clip with a tone plays in order, the game goes on to the title screen). It writes the "SofdecStream" packet (sector 2) but not CRITAGS, and neither the game nor the tests needed more; ffmpeg needs the Sofdec packet to see the audio as ADX.

## What a feature would need

- A tool, for example `tools/sfd_tool.py make <video> <out.sfd>`: asks the author's ffmpeg for MPEG-1 video (640x480 29.97, no B-frames, bitrate low enough for the file) and ADX audio (32 kHz stereo), writes the header sectors and interleaves video and audio sectors by time (the spike reuses a template's order instead), puts PTS on the first picture of each packet, patches the frame counts of the CRI packets, ends the file after the last sector. The port itself would not carry an encoder; the mod would ship the finished `.sfd` under `files/Data/movie/` (that already works as a whole-file replacement).
- Checks the spike did not make: a movie longer than the template (more sectors), B-frames, variable bitrate peaks against the player's buffer (the template's system header declares a buffer size), a movie without audio (two originals showed no audio stream in their first 4 MB), different frame rates, the other 58 movies' header differences, and the ADX loop fields.
- Size: the original movies are huge (up to 180 MB), so the replacement cost is the author's disk, not the game's memory: the file is streamed.
- Rights: the movies are the game's cutscenes; a replacement is a mod author's own content.

## Notes for the next session

- The harness needs `UC_GAME_DIR` set (or the usual `S:\Games\Umbrella Chronicles\game` folder) for tests that read the game's files; on 2026-10-23 the folder `game` was gone and `game_en` / `game_ru` existed instead, so spike runs passed `--game-arg=--game --game-arg=<folder>`.
