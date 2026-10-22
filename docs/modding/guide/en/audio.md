# Audio

[Русский](../ru/audio.md) · **English**

You are here: [Modding guide](README.md) › **Audio**

How to replace the game's music, voices and cutscene sound with ordinary `.ogg` or `.wav` files. You need no special tools: the port converts your file to the game's own format by itself.

## Contents

- [How it works](#how-it-works)
- [Quick start](#quick-start)
- [Finding the stream you want to replace](#finding-the-stream-you-want-to-replace)
- [Preparing your audio file](#preparing-your-audio-file)
- [Looping music](#looping-music)
- [What the port does with your file](#what-the-port-does-with-your-file)
- [Replacing a stream with a game-format file](#replacing-a-stream-with-a-game-format-file)
- [Changes while the game runs](#changes-while-the-game-runs)
- [Limits](#limits)
- [Troubleshooting](#troubleshooting)
- [See also](#see-also)

## How it works

The game keeps its music, voices and cutscene sound as about 1330 **stream files** named like `Data\sound\stream\00_Title.brstm` (the title music) or `VO_2201003.brstm` (one voice line). A mod replaces a stream by putting a file with the **same name** (without the extension) into its `audio` folder:

```
mods\my-music\
  mod.json
  audio\
    00_Title.ogg          replaces the title music
    VO_2201003.wav        replaces one voice line
```

When the mod is on, the port converts each file in the background, stores the result in its cache, and lays it over the game's stream. Until the conversion is done, the original keeps playing.

Short sound effects (button clicks, gun shots) live in sound banks, not in stream files; they cannot be replaced this way.

## Quick start

1. Make the folder `mods\my-music\audio`.
2. Copy your song there as `00_Title.ogg` (or `.wav`).
3. Create `mods\my-music\mod.json`:

   ```json
   { "id": "my-music", "name": "My title music", "version": "1.0", "api": "1.1" }
   ```
4. Start the game, press **Insert**, tick the mod in the **Mods** tab, and restart the game. The title screen plays your song.

Under the mod's name the Mods tab shows `converting audio (1 of 1)` while the conversion runs and any problems afterwards.

## Finding the stream you want to replace

The streams are in the game's data folder: `game\DATA\files\Data\sound\stream`. Names are not very descriptive but follow patterns:

| Pattern | What it is |
|---|---|
| `00_Title`, `01_Gameover`, `02_Scenario`, `03_Result` | Title, game over, chapter selection, results screen |
| `04_Train_...` and similar numbered names | Music of the missions |
| `VO_xxxxxxx` | One voice line each; mono, small |
| other names | Cutscene audio and special music |

To find out **which stream plays right now**, start the game from a command prompt with the `--log-dvd` option:

```bat
uc.exe --log-dvd
```

The console prints a line `[dvd] open Data/sound/stream/<name>.brstm` every time the game opens a stream. Play the scene you want, and read the name from the output.

## Preparing your audio file

**Formats the port reads**

- **Ogg Vorbis** (`.ogg`): mono or stereo, any sample rate.
- **WAV** (`.wav`): 8, 16, 24 or 32 bit integer, or 32 bit float; mono or stereo; any sample rate.

MP3, FLAC, AAC and more than two channels are not supported. Convert them first. With the free tools **ffmpeg** or **Audacity** this takes one step:

```bat
ffmpeg -i mysong.mp3 -ac 2 -c:a libvorbis -q:a 6 00_Title.ogg
```

(`-ac 2` makes stereo, `-q:a 6` is good quality; use `-ac 1` for a mono voice line.)

**Channels and sample rate** of the game's stream are fixed (mostly mono for voices, stereo for music; 32 kHz or 48 kHz). The port adapts your file for you: a stereo file for a mono stream is mixed down, a mono file for a stereo stream plays on both speakers, and any sample rate is converted. So you do not have to match them, but you get the best result with a file that is already stereo for music and mono for voices.

**Length:** a file may be up to 30 minutes and 512 MB.

**Loudness:** the port does not change the volume. Match your file to the loudness of the original so it does not stick out.

## Looping music

Music in the game loops: when it reaches the end it jumps back to a **loop start** and plays on. A game stream loops from the loop start to the very end of the stream, so the port cuts your audio at the loop end.

The loop comes from the first of these that exists:

1. **`mod.json`:** the `audio` entry for the stream, in samples of **your own file**:

   ```json
   {
     "id": "my-music", "name": "My title music", "version": "1.0", "api": "1.1",
     "audio": { "00_Title": { "loop_start": 1000, "loop_end": 900000 } }
   }
   ```

   `loop_end` is exclusive (the first sample after the loop). The start must be before the end.
2. **The file itself:** a WAV `smpl` chunk (the first loop; most audio editors can write it), or Vorbis comments `LOOPSTART` and `LOOPLENGTH` (or `LOOPEND`). For ffmpeg:

   ```bat
   ffmpeg -i song.wav -c:a libvorbis -q:a 6 -metadata LOOPSTART=100000 -metadata LOOPLENGTH=2000000 00_Title.ogg
   ```
3. **Nothing:** the stream plays once and stops.

To find sample numbers, open the file in an editor such as Audacity, select the loop, and read the sample positions (set the time display to samples).

A loop that does not fit the file (start not before end, end beyond the file) is ignored, and the Mods tab shows a note under your mod.

## What the port does with your file

1. Decodes your `.ogg` or `.wav`.
2. Resamples it to the stream's rate (32 000 or 48 000 Hz) and adapts the channels.
3. Applies the loop (cuts the audio at the loop end).
4. Stores it as an uncompressed 16-bit stream in `mods\.cache\_audio\<mod id>\`. The file is about **3.5 times larger** than the original stream (a 16 MB song becomes about 56 MB). Each converted file exists twice on the disk (the cache and a copy laid over the game).
5. Lays it over the game's stream, in front of the original.

The result is remembered. When you start the game again, a file that has not changed is not converted again. A file counts as unchanged when its **size and its modified time** are the same; if you edit a file in a way that keeps both, touch it or change its size.

## Replacing a stream with a game-format file

If you already have a proper `.brstm` file, put it in `files\Data\sound\stream\<name>.brstm` ([Disc files](files.md)). A `.brstm` in `files\` beats an `audio\` file with the same name in the same mod, and the Mods tab says that the audio file is not used.

## Changes while the game runs

- The game opens a stream when it starts playing it and **keeps reading the version it opened**. So a music track that is playing now stays the original until the track starts again.
- A converted file the game may already have opened is **never replaced under it**. If you change or delete a file in `audio\` while the game runs, the Mods tab notes it and the change takes effect at the next start of the game.
- Switching a mod off puts the original back from the next time the game opens the stream. The converted files stay on the disk, so switching it on again is instant.
- Old versions of an edited file are deleted once the new one is converted. The cache of a mod you removed is deleted at the next start of the game.

## Limits

- Only the streams (music, voices, cutscene audio). Sound effects from sound banks, and the sound of video files, are not replaceable this way.
- The name must match an existing stream. A file whose name matches no stream is ignored and reported in the Mods tab.
- Ogg decoding uses a third-party decoder written for trusted files: a damaged or hostile `.ogg` can crash the game. Only use files you trust and publish files you made ([safety reference](../../safety.md)).

## Troubleshooting

| Problem | Likely cause and fix |
|---|---|
| The original still plays | The name does not match (check with `--log-dvd`), conversion is not finished yet, or the stream was already open: restart the game. |
| The Mods tab says "the game has no stream named ..." | The file name is wrong. Names are case-insensitive but must otherwise match; letters, digits, `_` and `-` only. |
| "audio: ...: ignored: the mod's own files/.../x.brstm is used instead" | You also have a `.brstm` of the same name in `files\`. Remove one of them. |
| The Mods tab shows a red message about the file | The file is damaged or in an unsupported format (MP3, FLAC, more than two channels, a WAV with a strange layout). Convert it again with ffmpeg or Audacity. |
| The music does not loop, or loops at the wrong place | No loop information: add it in `mod.json` or the file. Check the sample numbers belong to **your** file. |
| Your file sounds too fast or too slow | Should not happen: the rate is converted. If the file itself has a wrong rate in its header, fix the file. |
| The disk fills up | Converted files are about 3.5 times the size of the originals. Delete `mods\.cache\_audio` to reclaim space (it is rebuilt when needed). |

## See also

- [Disc files](files.md) - ready game-format files and everything that is not audio.
- [Testing and sharing](testing-and-sharing.md) - testing, packing and publishing a mod.
- [Audio reference](../../audio.md) - the same facts in the technical reference.
- [mod.json reference](../../manifest.md) - the `audio` field.

Previous: [Textures](textures.md) · Next: [Text](text.md)
