# Replacing text

A mod changes what the game says with plain JSON files in `mods/<id>/text/`. The port patches the game's text files itself; no tools are needed to build anything.

```
mods/my-words/
  mod.json
  text/
    all.json       applies to every language the game has
    us.json        applies to the US text file only
    jp.json        ... one file per language: us en fr ge it jp sp du
```

Each file maps a record id to the new text:

```json
{
  "500": "Samurai Edge",
  "681": "First line\nSecond line"
}
```

`\n` starts a new line; the number of lines of the record follows the text (the game's texts are split into lines already: a record with two lines in the original has two lines in the dump). Keep `%s`, `%c` and `%d` where the original has them: the game fills them in.

## Finding the ids

`tools\dump_text.bat <file>` (next to the game) prints a text file as exactly this JSON:

```
tools\dump_text.bat "game\DATA\files\Data\message\mes_en.txtres" --output en-original.json
```

Cut the entries you do not want to change and edit the rest. The files are in `game/DATA/files/Data/message/`: `mes_us`, `mes_en`, `mes_fr`, `mes_ge`, `mes_it`, `mes_jp`, `mes_sp`, `mes_du`. The game picks one by its region, so a mod for a US game needs `us.json` (or `all.json`). Some localizations show their own alphabet through the font's code points (a Russian `mes_us` stores `Handgun` as `&ucmovem`): the dump shows the file as it is, and so must your text.

## Rules

- Order: for each mod, `all.json` first and then the language file; later mods win per id. A mod that ships its own `files/Data/message/mes_<lang>.txtres` gives the base the patches are applied to (the last such mod wins); otherwise the game's own file is the base.
- A line may hold at most 2048 bytes and a record at most 32 lines; text may not contain a NUL character; one file may hold at most 100000 entries; ids are written without leading zeros (`7`, not `007`). A Windows line end (`
`) in a text is read as `
`. The changed text file may not grow past 8 MB.
- The game's own texts are at most 252 bytes per line and 12 lines per record. A text far beyond that (more than twice the longest line or twice the most lines) is applied but listed in the Mods tab: the game's text boxes were made for the original sizes, and only a look in the game tells whether yours fits.
- Problems do not stop the mod: a bad entry (not text, too long), a file that is not valid JSON, or a language the game has no file for is listed under the mod in the Mods tab as `text: <file>: ...`, and the other entries still apply. Ids the file does not have are summarised in one line per file (`10 ids do not exist in mes_us.txtres (500, 501, ...)`), and at most 20 problems per mod are shown.
- Two mods that change the same id are not an error (the later one wins); the game's console output says `[mods] text id N is set by A and B (the last one wins)`.
- The game reads its text file once, when it starts, so a text change needs a restart of the game (a change made in the Mods tab while the game runs is prepared at once and used the next time the game opens the file).
- The placeholders (`%s`, `%d`, `%c`) of a record must match the original's, in the same order: the game passes some texts to `printf`, and a missing or extra one would crash it or print garbage. An entry that does not match is refused (the original text stays) and the Mods tab says so. Write `%%` for a percent sign: `100% damage` is read as the conversion `% d`.

The original files are never changed: the patched copies live in `mods/.cache/_text/`, one folder per version, and older folders are deleted at the next start. Subtitle tables (`movie_jimaku*.csvtbl`) are replaced as whole files through `files/` (see [files-and-textures.md](files-and-textures.md)).
