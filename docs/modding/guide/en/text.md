# Text

[Русский](../ru/text.md) · **English**

You are here: [Modding guide](README.md) › **Text**

How to change what the game says: item and weapon names, menu text, subtitles, dialogues, or a whole translation. You edit plain JSON files; the port patches the game's text files itself.

## Contents

- [How it works](#how-it-works)
- [Quick start](#quick-start)
- [Finding the ids](#finding-the-ids)
- [The JSON file](#the-json-file)
- [Languages and files](#languages-and-files)
- [Placeholders - percent signs](#placeholders---percent-signs)
- [Length limits](#length-limits)
- [Several mods](#several-mods)
- [Special case - a localization with its own alphabet](#special-case---a-localization-with-its-own-alphabet)
- [When the game notices a change](#when-the-game-notices-a-change)
- [Problems the Mods tab reports](#problems-the-mods-tab-reports)
- [Troubleshooting](#troubleshooting)
- [See also](#see-also)

## How it works

The game's texts live in one file per language, `Data\message\mes_<language>.txtres`: `us`, `en`, `fr`, `ge`, `it`, `jp`, `sp`, `du`. Each file holds about 2600 **records**; every record has a number (its **id**) and one or more lines of text.

A text mod lists the ids it wants to change and the new text. The port takes the game's own file, writes your texts into a **copy**, and shows that copy to the game. The original file is never touched.

## Quick start

We will rename the handgun in the HUD.

1. Find its id: records 500, 550 and 553 hold the name "Samurai Edge 2" (see [Finding the ids](#finding-the-ids)).
2. Make the folder `mods\new-names\text`.
3. Create `mods\new-names\text\all.json`:

   ```json
   {
     "500": "Silver Pistol",
     "550": "Silver Pistol",
     "553": "Silver Pistol"
   }
   ```
4. Create `mods\new-names\mod.json`:

   ```json
   { "id": "new-names", "name": "New names", "version": "1.0", "api": "1.0" }
   ```
5. Start the game, tick the mod in the **Mods** tab and **restart the game** (the text is read once at start). The HUD shows the new name.

## Finding the ids

Use the dump tool that is installed next to the game:

```bat
tools\dump_text.bat "game\DATA\files\Data\message\mes_en.txtres" --output en-original.json
```

It writes the whole text file as JSON in exactly the format a mod uses:

```json
{
 "1": "Enter",
 "2": "Pick Up",
 "500": "Samurai Edge 2",
 "681": "This crank with a square point was used\nto open shutters and flood gates."
}
```

Open the file in any text editor, search for the words you see in the game, and note the ids. Then **delete the entries you do not want to change** and edit the rest: your mod's file lists only what you change.

The English file (`mes_en`) is the easiest to read. The ids are the same in every language, so you can find an id in English and change it in the language the game uses.

## The JSON file

```json
{
  "500": "Silver Pistol",
  "681": "First line\nSecond line"
}
```

- Keys are **record ids** written as text, without leading zeros (`"7"`, not `"007"`).
- Values are the new text. **`\n` starts a new line**; the number of lines of the record follows your text. A record that has two lines in the original has two lines in the dump.
- Write the file in **UTF-8**. A Windows line end (`\r\n`) is read as `\n`.
- You can use any characters the game's font can draw; characters the font lacks are shown as blanks or wrong symbols.

## Languages and files

The mod's `text` folder may contain these files:

| File | Applies to |
|---|---|
| `all.json` | Every language file the game has |
| `us.json`, `en.json`, `fr.json`, `ge.json`, `it.json`, `jp.json`, `sp.json`, `du.json` | That one language only |

Inside one mod, `all.json` is applied first and then the language file, so a language file can refine `all.json`.

**Which language does the game use?** It picks the file by the region of the game: a US game reads `mes_us`. So a mod for a US game needs `us.json` (or `all.json`).

## Placeholders - percent signs

Some texts contain **placeholders** such as `%s`, `%d` or `%c`. The game replaces them with a name or a number when it shows the text. For example the original may read `Got %s!`.

**Your text must contain the same placeholders in the same order.** The game passes some texts to a formatting function, and a missing or extra placeholder would make it crash or print garbage. If your entry does not match, the port refuses it (the original text stays) and the Mods tab tells you which id and why.

- To write a literal percent sign, double it: `50%% done`.
- Be careful with a `%` followed by a space and a letter: `100% damage` is read as the conversion `% d`. Write `100%% damage`.

## Length limits

- A line may be at most **2048 bytes**, a record at most **32 lines**, one file at most **100 000 entries**; the changed text file may not grow past 8 MB. Text may not contain a NUL character.
- The game's own texts are at most 252 bytes per line and 12 lines per record. The game's text boxes were made for those sizes. A text far beyond them (more than twice the longest original line, or more than twice the most original lines) is still applied, but the Mods tab lists a note, because only a look in the game tells whether it fits.

## Several mods

When two mods change the same id, **the lower mod in the Mods tab list wins**. The game's console output says `text id N is set by A and B (the last one wins)`; inside one mod, `all.json` and a language file do not count as a conflict.

A mod can also ship its **own complete text file** as `files\Data\message\mes_us.txtres` ([Disc files](files.md)). That file then becomes the **base** to which the text patches of all mods are applied (the last mod that ships one wins).

## Special case - a localization with its own alphabet

Some localizations of the game (for example a Russian one stored in `mes_us`) show their own letters through the font's code points: the file really contains the Latin-looking text `&ucmovem` for what is displayed as "Pistol" in the local alphabet. The dump shows the file exactly as it is, and your text must use the same code points. If you want to translate such a game, first look at how the original strings look in the dump, and write your strings in the same encoding.

## When the game notices a change

The game reads its text file **once, when it starts**. So a text change needs a restart of the game. If you change a mod's text while the game runs, the port prepares the new file at once and the game uses it the next time it opens the file, which for text is the next start.

## Problems the Mods tab reports

Problems never stop a mod: a bad entry is skipped and the other entries still apply. They are listed in red under the mod as `text: <file>: ...`:

| Message | Meaning |
|---|---|
| `not valid JSON` | A comma, bracket or quote is missing. Check the file in a JSON validator. |
| `must be a JSON object of record ids to text` | The file must start with `{`. |
| `id "..." is not a record id` | The key is not a whole number. |
| `id "007" has leading zeros` | Write the id without leading zeros. |
| `id "..." must be text` | The value must be a string (in quotes). |
| `10 ids do not exist in mes_us.txtres (500, 501, ...)` | The file has no record with these ids (the first five are listed). |
| `the placeholders (%s, %d, ...) must match the original's` | See [Placeholders](#placeholders---percent-signs). The original text stays. |
| `much longer than the original text file has` | The text is applied, but may not fit the text boxes. |
| `more than 32 lines`, `a line is longer than 2048 bytes` | Over the limits. |
| `the game has no text file for the language 'xx'` | The game has no file for that language (the Dutch file has fewer records than the others). |
| `more problems are not listed` | At most 20 problems are shown per mod. |

## Troubleshooting

| Problem | Likely cause and fix |
|---|---|
| Nothing changes in the game | The mod needs a **restart** of the game. Also check the language file: a US game reads `us`, so `en.json` alone changes nothing there. |
| Only some texts change | The same words are stored under several ids (for example `500`, `550` and `553` for one name). Search the dump for every occurrence. |
| The game crashes when a text shows | A placeholder is missing or extra (the port normally refuses those), or a lone `%` was written. |
| Letters look wrong | The font lacks the characters, or the localization uses its own code points ([special case](#special-case---a-localization-with-its-own-alphabet)). |
| The text is cut or overlaps | It is too long for the text box. Shorten it. |

## Short character names in `main.dol`

Eight short names (Ada, Billy, Carlos, Chris, HUNK, Jill, Rebecca, Wesker) are not in the text file: the game keeps them inside `main.dol`, next to the internal labels `HG_A`... `HG_G`. A mod changes them with `dol/names.json`:

```
mods/my-translation/
  mod.json
  dol/names.json
```

```json
{ "Ada": "Ада", "Billy": "Билли", "Carlos": "Карлос", "Chris": "Крис", "HUNK": "ХАНК", "Jill": "Джил", "Rebecca": "Ребекка", "Wesker": "Вескер" }
```

- The key is the original English name; the value is the new text in UTF-8, as in the text files. The names in `main.dol` are only 3 to 7 bytes long, but the game copies each one into a 16-byte field of its character slot, and the port puts your text there instead of the original: a name may be **up to 15 bytes** (7 Cyrillic letters, which take 2 bytes each). It is used the next time the game sets its characters up (a chapter start).
- A font that draws its letters on other codes (for example Cyrillic on Latin codes) needs a table, `dol/charmap.json`: each character and the code the font draws it with, either one character or a whole number from 1 to 255:

  ```json
  { "К": "K", "р": "p", "и": "u", "с": "c", "ж": 126 }
  ```

  A mapped character becomes that one byte; every other character stays its UTF-8 bytes. Without a `charmap.json` the text is written as it is.
- A name that is unknown, empty, longer than 15 bytes, or sits in a `main.dol` whose table is somewhere else is listed in the log as `name patch: ...` and skipped; the other names still apply. Switching the mod off puts the original names back; when two mods name the same character the later one wins.
- Longer names (and every name the game shows in a line of text) belong in the text file, for example record 252, "Chris Redfield".
- Together with a font in `files/Data/sysres/font0/` and `font1/` (see [Disc files](files.md)) this lets a translation work on the plain English game, without an edited `main.dol`.

## See also

- [Disc files](files.md) - replacing subtitle tables and other files as a whole.
- [Testing and sharing](testing-and-sharing.md) - testing, packing, publishing.
- [Text reference](../../text.md) - the same rules in the technical reference.

Previous: [Audio](audio.md) · Next: [Disc files](files.md)
