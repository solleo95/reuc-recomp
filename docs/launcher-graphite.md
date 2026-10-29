# Graphite launcher — 2026-10-14

User selected concept 1. Native C++ / SDL3 / ImGui; no additional UI framework.

## Behavior

- Graphite palette, train banner, persistent sidebar and launch/save footer.
- Game, Graphics, Controls, Controllers, Audio sections retain existing settings and binding editor. Controls (keyboard and mouse) has the mouse sensitivity slider at the top; Controllers holds the control type, the gamepad model and the **Gamepad** and **Wii Remote** windows (`launcher/gamepad_panel.*`, `launcher/wiimote_panel.*`, shared with the in-game menu). The pointing device (mouse or light gun), aim smoothing and the reload zone are on Controls. See [lightguns.md](lightguns.md).
- Language selector applies immediately. Save (or Play) persists it to `launcher.language` in `config.json`.
- Default `system` resolves the Windows preferred UI language at startup. Unsupported language falls back to English.
- Languages: en, ru, de, fr, es, it, pt-BR, pl, uk, tr, ja, ko, zh-Hans.
- Language affects the launcher only. It does not change game assets, region, save data or the game's language.
- Existing atomic config saves and stale-file detection preserve other settings and bindings.
- File picker selects the extracted game directory. Play requires `DATA/sys/main.dol`.
- Missing/corrupt decorative banner shows a translated fallback; settings remain available.
- Audio page explains the current Windows output/mixer behavior. DLSS/FSR controls from the concept await implementation of those features.

## Assets and fonts

The launcher's artwork is two PNG files in `assets/launcher/`, both installed next to `uc_launcher.exe` and both required by the release check: `graphite-train.png` (2164x727) is the header, scaled to cover the header area (the overflow is cut equally at the edges); `graphite-brand.png` (342x502) holds the sidebar logo at (0,0)-(342,446) and the company footer at (0,446)-(304,502). The README banner uses `graphite-train.png` too. Navigation icons are transparent vector geometry shared by selected/unselected states. Interactive controls and localized text are rendered independently. Original concepts are kept locally and excluded from Git.

Layout follows the concept's 1504×916 client area, 344-pixel sidebar, 232-pixel header and two settings columns. Thin outlined fields, chevrons, round toggles and footer proportions follow the reference. Unimplemented upscaling, quality and frame-generation controls are disabled and marked unavailable. Render-scale choices: monitor resolution, 1× (Native), 1.5×, 2×, 3×, each with output dimensions. Existing custom config values are preserved until a new choice is made.

Georgia large headings, Segoe UI labels/controls/status, and available Microsoft YaHei, Malgun Gothic and MS Gothic fonts are loaded from Windows, not redistributed. Text sizes are rounded to pixels; language text uses the same size as settings. East Asian coverage depends on installed Windows font packages. Both font families are checked against all catalog glyphs on the development machine. Translations still need native-speaker proofreading.

## Verification

- `build.bat --no-install`: 80/80 CTest tests pass.
- `test_launcher_locale`: all locale tags, English fallback, actual Windows UI language, all catalog strings nonempty, config roundtrip, unknown-field preservation and invalid-input rejection. Actual Windows language resolved to `ru`.
- `build/graphite-smoke-5`: UI clicks for bindings, clear/capture/defaults, stale-save error, both prompt styles, all 13 languages with save/reload, all translation glyphs, all pages at 1000×720, launch of an isolated 20-second game with copied saves. No OS input injection.
- `build/graphite-no-art-red`: missing banner originally prevented startup.
- `build/graphite-no-art-green`: same case now passes all UI checks with a diagnostic fallback.
- Faithful-concept revision: `build/graphite-faithful-final` passes the same interactive smoke including isolated game launch, 13 languages, both font families, save/reload and minimum-window checks. Current screenshots: `build/graphite-faithful-final/ru.bmp`, `de.bmp`, `ja.bmp`, `small-*.bmp`.
- Native Windows folder-picker interaction was not automated. No browser renderer is involved.
- Scoped review's claim that the Windows size query always fails was rejected: official API contract and a direct runtime comparison confirm correct behavior.

No commit made. Installed config and saves are preserved during `/E` deployment.

Installed faithful revision verified by matching executable/atlas hashes and `build/graphite-installed-check-2` interactive smoke with isolated game launch (passed). The earlier stale-save assertion failure was traced to reused smoke data: writing `true` again did not change the file. Smoke now toggles the marker; repeated runs against the same directory pass.

## UI corrections — 2026-10-14

- Reproduced popup bug with a failing anchor assertion (`launcher-fixes-red`): `BeginCombo` changes the current window before custom label/arrow rendering. Frame geometry now comes from the parent before opening the popup.
- Transparent vector navigation icons, stable icon bounds across states, pixel-aligned panel junction.
- Folder input/button gap and centered folder glyph; larger language selector; padded, striped binding rows with frozen headers and adjacent edit buttons where space permits.
- `build/launcher-ui-final`: interactive checks of all languages, bindings, styles, window-mode/scale selection and save, popup captures, minimum 1000×720 window. Repeated same-directory run passes.
- Windows DPI runtime check: per-monitor aware, DPI 120 (125%), framebuffer 1500×913. Other Windows display-scale settings and moving between monitors were not exercised.
- `build.bat --no-install`: 80/80 tests pass after corrections.

## Binding editor follow-up

- Controls page omits the train header and folder/status block to give the table more height; Play/Save remain at the bottom. Other pages retain the folder picker.
- Removed shared-binding notices and the explicit Assign Esc buttons. Esc is captured as physical scancode 41, like other keys; losing focus still cancels capture. Updated capture hints in all 13 languages.
- Keyboard labels use SDL scancode names (W/S/R/G/V) rather than layout-dependent keycodes. The game already consumes scancodes; no runtime binding-format change is required.
- Switches replaced with native checkboxes with centered labels and an explicit label gap.
- Regression: `test_binding_editor` first failed for Esc capture, then passed. UI smoke now captures/saves Esc and checks physical key names. Existing `test_host_input` verifies scancode handling when keycodes differ.

## Compact binding grid

- Four aligned columns follow the supplied launcher reference: action, centered binding list, `+`, clear. At 1500×913 about ten actions fit without scrolling.
- Clicking the binding list starts replacement; `+` appends. Replacement is transactional: previous bindings survive focus-loss cancellation and validation errors. Invalid capture remains active for another attempt.
- Right-click a binding list to remove one entry. Special grenade/knife/QTE actions expose physical capture and their gesture/chord through the `+` popup.
- Unit checks cover replacement, cancellation, invalid input, and append after replacement. UI smoke exercises replacement and saving as well as the existing clear/add/Esc paths.
