# Release Notes — In-game language selection

## Added
- The launcher's language now also selects the original game's text, fonts and localized interface resources on the next game launch.
- Native packs: English, German, French, Spanish, Italian and Japanese, when present in the extracted game files.
- Spanish text and accented characters work on the NTSC-U disc (RBUE08) without copying Spanish files over English files.

## Fallback and compatibility
- Unsupported languages and incomplete language packs fall back to US English resources.
- Pack checks include messages, fonts, localized interface and attract-screen archives, HomeButton resources and the appropriate save banner.
- Network play, deterministic runs and demo recording/playback always use English game resources. The launcher and port UI keep their selected local language.
- Original disc files, region and save data are not rewritten. Existing translations stored inside English resources remain unchanged.
- Language-specific text mods continue to target their own files: for example, Spanish uses `sp.json`, while `all.json` applies to all available language files.

## Usage
Select a language in the launcher, save or start the game, and restart any already running game for the change to apply. No separate in-game text-language setting is required.

## Verification
- Windows AVX2 and compatible CPU builds produced and staged successfully.
- Full CTest suite: 257 tests passed; pre-integration repeat also passed 257/257.
- Live checks: Spanish title and gameplay, accented Ó, English resources, unsupported-language fallback, English deterministic/demo modes, and a network pair with English/Spanish launcher settings using English game resources and matching confirmed state hashes.
- Independent final review approved the change after repairing incomplete-pack dependency checks.

## Limits
- Dutch files exist on the inspected disc, but do not form a supported complete native pack for this selector.
- German, French, Italian and Japanese mappings and availability are tested; their rendered text has not been visually checked in a live run.
- Linux/Steam Deck and the compatible CPU executable have not received a live language smoke test.
- Selecting US English uses the installed US resource files as-is; it does not remove a translation already patched into those files.
