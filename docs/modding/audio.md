# Replacing music and voices

Put an ordinary audio file in `mods/<id>/audio/` and the game plays it instead of the stream with the same name. No tools are needed: the port converts the file itself.

```
mods/my-music/
  mod.json
  audio/
    00_Title.ogg        replaces sound/stream/00_Title.brstm (the title music)
    VO_2201003.wav      replaces one voice line
```

## Names

The file name without its extension is the stream's name on the disc, case does not matter: `audio/00_title.ogg` and `audio/00_Title.OGG` both replace `Data/sound/stream/00_Title.brstm`. The game has about 1330 streams (music, voices, cutscene audio); the Mods tab lists a file whose name matches no stream as a problem and ignores it. If `audio/` holds `a.ogg` and `a.wav`, the one that sorts first is used and the other is reported.

A ready game file under `files/Data/sound/stream/<name>.brstm` in the same mod wins over `audio/<name>.*`. Between mods the load order decides, as for every file.

## Formats

- Ogg Vorbis (`.ogg`), mono or stereo, any sample rate.
- WAV (`.wav`): 8, 16, 24 or 32-bit integer or 32-bit float, mono or stereo, any sample rate. Extra chunks are skipped.

Not supported: MP3, FLAC, more than two channels, files longer than 30 minutes or larger than 512 MB. The reason appears in the Mods tab under the mod.

## What the port does with the file

The game's stream has a fixed sample rate (32 or 48 kHz) and channel count (mostly mono for voices, stereo for music). The port resamples your file to that rate and converts the channels: a stereo file for a mono stream is mixed down, a mono file for a stereo stream plays on both channels. It stores the result as a 16-bit PCM stream, about 3.5 times larger than the original (a 16 MB song becomes about 56 MB in `mods/.cache/<id>/`). Converted files are cached; changing the source converts it again.

Conversion runs in the background when the game starts or the mod is switched on, and the Mods tab shows `converting audio (1 of 3)`. Until a file is done the original plays. The game opens a stream when it starts it, so a stream that is already playing keeps the original until it starts again (for music: the next time the track begins).

## Looping

A BRSTM loop always runs from the loop start to the end of the stream, so the port cuts your audio at the loop end. Where the loop comes from, first match wins:

1. `"audio"` in `mod.json`: `{ "audio": { "00_Title": { "loop_start": 1000, "loop_end": 900000 } } }`, in samples of your own file (end exclusive).
2. The file itself: the WAV `smpl` chunk (first loop), or the Vorbis comments `LOOPSTART` with `LOOPLENGTH` (or `LOOPEND`).
3. None: the stream plays once.

A loop that does not fit the file (start not before end, end beyond the file) is ignored and the Mods tab says so.

## Switching off

Switching the mod off, or removing a file from `audio/`, removes the converted stream; the original plays again from the next time the game opens it.
