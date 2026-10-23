# Replacing a movie

The game's 59 movies (`Data\movie\*.sfd`, 2.4 GB in all) play through the game's own decoder, so a mod replaces one by putting a finished movie file at the same path: `mods\my-mod\files\Data\movie\MV_000_0.sfd`. The file is a whole-file replacement like any other (see [Files](guide/en/files.md)); the port does not convert anything. `tools\sfd_tool.bat` makes the file from an ordinary video.

## Making a movie file

You need [ffmpeg](https://ffmpeg.org) (a full build: it must have the `mpeg1video` and `adpcm_adx` encoders; the usual Windows builds do) on `PATH` or given with `--ffmpeg`.

```
tools\sfd_tool.bat make my-intro.mp4 MV_000_0.sfd
tools\sfd_tool.bat make wide.mkv MV_000_0.sfd --fit crop --bitrate 3500 --force
tools\sfd_tool.bat info "game\DATA\files\Data\movie\MV_002_0.sfd"
```

| Option | Meaning |
|---|---|
| `--fit pad` (default) | Keep the aspect ratio (as it is seen: non-square pixels of DVD-style video are taken into account) and fill the rest of the 4:3 picture with black bars. |
| `--fit stretch` | Stretch the picture to 640x480. |
| `--fit crop` | Fill 640x480 and cut off what does not fit. |
| `--bitrate KBPS` | Picture bitrate, 50 to 19000 (default 2500; the game's own movies are about 1700). |
| `--ffmpeg PATH` | Use this ffmpeg. |
| `--force` | Overwrite an existing output file. |

What the tool does: ffmpeg turns the picture into MPEG-1 video (640x480, 29.97 frames a second, no B-frames) and the sound into ADX (stereo, 32 kHz), and the tool writes them into the game's file layout. A video without sound gets silence; a sound longer than the picture is cut and a shorter one is padded with silence. Movies longer than 30 minutes are refused. `info` shows what any `.sfd` holds, the game's own included.

## Which movie plays where

The names are `MV_<chapter>_<n>.sfd` (cutscenes) and a few others. Start the game with `uc.exe --log-dvd` and watch for `[dvd] open Data/movie/...` lines to see which file a scene opens. `MV_000_0.sfd` is the opening movie after the logos.

## Limits and notes

- The picture is always 640x480 4:3 on the game's side: the game stretches it to the window like its own movies.
- The game plays the file until it ends and then goes on, so a movie of any length up to the limit works; the original subtitles of a scene are drawn over your picture and follow the game's own timing.
- Not checked: B-frames, very high bitrates (the decoder's buffer is sized for the originals), other frame rates (the tool always makes 29.97), movies that the game cuts short itself (skipping works as for the originals).
- A movie is the author's own content; the game's movies are Capcom's: ship only what you may.
- The file is read from disk while it plays, so a mod with a long movie costs disk space, not memory. The game keeps the version of a movie it opened: switching the mod off while the movie plays does not cut it.
