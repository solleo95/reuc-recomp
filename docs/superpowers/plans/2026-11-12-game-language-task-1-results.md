# Task 1 — Language contract evidence

## Proven native selector
Read-only developer investigation and parent source/xref check agree: `0x80242984` calls console language and compares app ID against caller's `RBUE`. Equality chooses English index2, French index5, Spanish index7. French5/Spanish7 generic suffix tables still resolve `_us`; only fonts become F/S. Thus a global SCGetLanguage override is insufficient.

Direct resource indices: JP0, EN(US)2, GE3, FR4, SP6, IT8. Native selector writes object+4; getter `0x80242AA8` reads it. The caller ignores the return value. OSGetAppGamename inside the original selector also refreshes a five-byte guest buffer (four application-ID characters plus NUL at r13-26624): replacement must account for this side effect rather than claiming bitwise equivalence of the whole function. A focused source investigation corrected the earlier six-byte report. The source is lowmem 0x80003180 when the SDA flag at r13-26664 is zero, otherwise the pointer stored at r13-29256. Preserve this alternate-source branch as well.

Actual disc read-only scope: `S:\Games\Umbrella Chronicles\game\DATA`, header RBUE08. Spanish `mes_sp.txtres` is 238080 bytes. Parent verified S font0/font1/font2 each 131720 bytes and localized option/saveload/select/gameover/result/dialog/game archives and movie_jimaku_sp.csvtbl exist. JP uses mes_jp, unsuffixed layout archives and only J font0. Follow-up developer xref and parent independent `--disasm 8003F938 39` (exit0) prove the index0 branch calls font loader once at8003F960, then copies that font slot via801B159C twice. Non-JP calls the loader for font0/1/2 at8003F998/8003F9B0/8003F9C4. Therefore Japanese requires no J font1/2 or sjis alternate. Follow-up bounded DOL investigation located Atract formatter `Data/layout/atract%s%s.arc` at803DD224 and suffix strings in native index order. Both aspect variants4_3/16_9 use jp/us/ge/fr/sp/it for selected ja/en/de/fr/es/it. Real en/es opens corroborate the mapping; indirect caller not pinned. HomeButton table803F5388 is consumed by800A762C using the same guest index: homeBtn.arc (JP), homeBtn_ENG/GER/FRA/SPA/ITA.arc. Banner table803F9450 maps JP savebanner_jp.arc, other indices savebanner_us.arc; caller not pinned. These localized dependencies must participate in complete-pack fallback. Shared HomeButton common resources do not distinguish a language pack.

## Commands and source attribution
- Worker xref inspected selector, suffix table, font table and callers; all commands reported exit0. Parent independently ran `build/tools/xref/xref.exe --dol <DATA/sys/main.dol> --disasm 802429E4 8`, exit0. It shows cmp0/bne then store2 on equal, store1 otherwise. This corrected the worker's initially inverted branch report.
- Parent read generated selector body and sole caller in `generated/generation_cf247ed612f02ea7/game_062.cpp:3504-3600` and `game_001.cpp:8312-8323`.
- Parent Get-Item/Get-ChildItem checked actual Spanish messages, fonts and seven layout archives, exit0.
- Parent baseline `pwsh -NoProfile -File scripts/ctest-hidden.ps1 -j 30`, exit0: 255/255 pass, 32.58 seconds, before implementation.

## Network boundary
Source proof: `game/game_checkpoint.cpp:85-87` combines all rollback RAM with scheduler state. `game/netplay_game.cpp:604-613` captures/hashes boundaries every60 steps; confirmed peers compare in `confirm`, two differing boundaries cause desync. Localized resource bytes and sizes differ; messages are loaded into hashed MEM2. Native different indices therefore cannot yield equal full guest state.

Existing presentation-only prompt annotations never change guest memory. However `gx_prompt_attach` accepts exactly one quad; a text paragraph has many. Old game-used Print entries `0x803B8F10` and `0x803BAE20` expose writer/string/count but no message record identity. Formatted char strings and converted wchar TextBoxes have already lost that identity. Native Print also advances writer state and computes metrics; replacing its guest behavior would not stay canonical.

Unknown host contract: a full-run renderer annotation/replacement with record/template identity, dynamic substitutions/control codes, font coverage, localized metrics/wrapping and image-text handling. A bounded rescue probe was assigned only to establish this contract, not implement a parallel UI system.

Superseded ruling: offline implementation initially proceeded while independent netplay localization was unresolved. The user subsequently revised current scope to force English for both network peers. Independent network localization is now excluded; no renderer changes or checksum omissions are needed.

Review required: cross-component resource selection and replay/netplay mode boundary; inspect incomplete-pack fallback, mods precedence, selector side effects, and real forced-English network startup evidence.
