# Game text language follows launcher language

## Intent and approved behavior
The existing `launcher.language` selects both the port UI and the original game's local text and fonts. Resolve `system` with the existing locale resolver. No second selector, new translation, disc rewrite, or change to saves/region. Apply at game startup, not live.

Use a requested language only when its required text, fonts (font0–2 except the verified Japanese font0-only path), localized layout/attract archives (both aspect variants), HomeButton language archive and native save-banner variant exist. Otherwise use English. Existing English/Russian-patched English files and language-specific text mods retain their current meaning. Do not manufacture translations or rewrite the installed disc.

Deterministic offline runs, demo recording/playback, and network play use English regardless of launcher language. User explicitly revised the scope after discovering native localized assets change shared guest state: independent network localization is deferred. Both peers retain local port UI language, but game text/fonts use English.

## Approaches
1. Native console-language selection: preferred for ordinary play. Set the language returned to the guest, letting its own tables choose messages, fonts, layouts, and regional variants. Verify the actual DOL mapping and disc inventory first.
2. Alias English asset paths: rejected as the primary solution because text/font-only substitution misses localized archives and complicates mod bases.
3. Presentation-only local substitution: deferred. It requires a full-string rendering/formatting boundary absent from the current prompt-overlay contract. No renderer translation subsystem or checksum exclusions are part of this task.

The user approved native-language selection, English demos, then explicitly selected forced English in netplay for the current version.

## Constraints and anti-goals
- No changes to disc/application identity, save namespace, netplay validation strength, or gameplay logic merely to select a language.
- Never mask a real desync by disabling hashes or skipping broad RAM ranges.
- Preserve mod overlay ordering and per-language text mod targeting.
- Reuse the existing shared config and locale resolution; no new CLI flag unless a test truly cannot use isolated config.
- Windows builds must stage both CPU variants through the existing build. No install/deploy or commit/push.
- All live checks use the existing harness, copied saves/config, visible nonactivating 1280x720 windows, and unlimited FPS. Automated commands run at Idle priority.
- External read-only data scope: `S:\Games\Umbrella Chronicles\game\DATA`. No installed-file modifications.

## Acceptance
- Spanish config loads actual Spanish messages and S fonts on RBUE08; inspect a captured accented text frame.
- Other verified disc languages use their mapped resources. Missing/partial resources fall back consistently to English. Unknown launcher languages use English.
- Offline deterministic/demo runs stay English independent of launcher language.
- Real network peers with different launcher configs both load English game resources, preserve confirmed-state hash equality and normal rollback/desync checking, and retain their own local port UI language.
- Focused tests cover mapping, availability/fallback, startup mode rules, and existing mod behavior. Run the full project tests and build; independently review cross-component and netplay risks.

## Verified native contract (RBUE08)
The original selector `0x80242984` receives the language object in r3 and a comparison ID in r4. Its caller supplies `RBUE`; the real disc header matches. Consequently SC English/French/Spanish become guest indices 2/5/7, and generic resources at 5/7 still use `_us`. Overriding SCGetLanguage alone cannot enable Spanish text. Override the game's selected resource index, retaining disc identity and required original side effects.

Direct resource indices: Japanese 0 (special `mes_jp`, unsuffixed layouts, J font0), English US 2 (`_us`, E), German 3 (`_ge`, G), French 4 (`_fr`, F), Spanish 6 (`_sp`, S), Italian 8 (`_it`, I). Dutch index9 still chooses `_us`; no Dutch launcher language exists, so do not add one. Non-Japanese fonts E/G/F/S/I exist in font0–2. Japanese has no J font1/2; verified startup branch loads font0 J once and copies its font object to slots1/2, so availability requires only font0 J. Non-Japanese title uses `title_us.arc`.

Installed messages, layouts, and font sizes differ by language, so native different-language guest RAM cannot pass the existing full-memory network hash. This is a concrete architecture constraint, not a reason to weaken validation. Parent baseline hidden CTest: 255/255 passed before feature changes.

## Feasibility gate
Native mapping and full-memory network hash boundaries are established read-only. Implement test-first native selection and actual harness integration. All state-equality modes select English through one tested policy; prove the real network startup path rather than infer correctness only from unit tests. Independent network text rendering is no longer a required feasibility gate.
