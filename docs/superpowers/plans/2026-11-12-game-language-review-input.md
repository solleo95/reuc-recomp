# Game language final review input

Review required: cross-component guest resource selection, mods ordering and deterministic/netplay compatibility; inspect selector side effects, missing-pack behavior and the actual forced-English startup path. No earlier independent review covers these changes.

## Requirements
Approved spec: `docs/superpowers/specs/2026-11-12-game-language-design.md`.
Normal play follows launcher.language (including existing system resolver) for available complete native EN/DE/FR/ES/IT/JA packs, else native US English. Do not rewrite disc/save identity, add second selector or change voice language independently. Netplay, deterministic/det-out and demo record/replay force English; user explicitly deferred independent netplay localization. Preserve per-language text mod targeting and overlay order. No weakened hashes, installed-file changes, commits or deployment.

## Changed source/test manifest and provenance
Initial working tree clean on feature/languages; all following tracked/new changes belong to this task. No committed-base comparison is requested; current source and task acceptance are primary.
- New runtime/game_language.h, runtime/game_language.cpp, runtime/hle/game_language.cpp.
- Modified runtime/CMakeLists.txt, project/hle_map.txt, game/uc_main.cpp (startup language resolution/selection before mods; existing UI reuses resolved locale).
- New tests/test_game_language.cpp; modified tests/CMakeLists.txt.
- New scripts/uctest/language.py, tests/test_uctest_language.py.
- Modified docs/launcher-graphite.md, docs/modding/guide/en/text.md, docs/modding/guide/ru/text.md, docs/modding/text.md, docs/spikes/steamdeck-windows-validation-backlog.md.
- Task spec/plan/briefs/results under docs/superpowers/2026-11-12-game-language* paths; worker reports in .superpowers/sdd/2026-11-12-game-language/. No source outside manifest accepted.
Connected code: runtime/dvdfs.{h,cpp}, game/game_mods.cpp, launcher/localization.cpp, game/options.cpp, game/game_checkpoint.cpp, game/netplay_game.cpp, generated/generation_8aea9ebc21cec047/game_001.cpp (actual HLE caller). Read bounded connected portions, not whole generated tree.

## Decisive verified contracts
Parent independently read original generated selector and xref'd DOL: on RBUE08 SC Spanish maps guest7, generic suffix_us. Direct guest6 selects_sp/S; French4, German3, Italian8, Japanese0. English2 preservesUS/patchedUS. Getter80259320 writes4 appID bytes+NUL using alternate-source branch; HLE preserves byte-order side effect for EN/FR/ES. Parent xref verified Japanese font0-only loader/copies, nonJP3fonts. Evidence details: task-1-results doc.

## Commands and evidence
- Worker RED missingimplementationlink failure; focused13runtimechecks GREEN. Parent independently reran runtimecheck exit0.
- Full build `build.bat --no-install`: compiled translated binding + AVX2 and compat, stageddist, skippedinstall. Initial task3workerfailed mid-runningbuild; process survived, recoveredlog reaches stage7. Original top-level process exit UNAVAILABLE, not claimed0. build/build-language.out.log lines648-661 show257tests passed after updatercleanup retry; build-language.err.log empty. Parent confirmed generated caller invokesHLE and hle.cfg mapping.
- Parent fullhiddenCTest afterbuild exit0,257/257 firsttry31.85sec.
- Later parent fullhiddenCTest after harness finalrepair exit8,256/257: test_sched.cpp49 wake_one==mid. Parent isolated test_sched repeated20 times allpass12.17sec exit0. Read-only timing investigation pending; scheduler/test source untouched. Do not describe latestfullsuite as whollygreen; evaluate attribution and boundedrisk. No unrelatedfixes yet.
- Parent Pythonharnessunits initially10pass; recoveryworkerfinal11pass (freshfullCTest also includesgreen languageunits).
- All7 live casesgreen per task3report and JSONs: Spanish+gameplay, en, ru fallback, deterministic es->English, det-out es->English with activeboundedtrace, demo record/replayes->English, netpairconfigen/es->bothEnglish. See .superpowers/sdd/2026-11-12-game-language/task-3-report.md for exactrunpaths and commandevidence.
- Parent inspected real Spanish titlecapture: `build/autotest/language-1794488353372882800/es/frame-1794488362687248-25588-8844.png` visibly PULSA CUALQUIER BOTÓN; Ó correct. Gameplaycapture also normal. Parent independently grep/read actual Spanish opens includesdialog_sp,game_sp,atract_4_3_sp,mes_sp/Sfonts; sharedtitle_us is expected (workerreport statementno_usarc is beingcorrected).
- Network report build/autotest/language-1794488874082961300/report.json: hosten/clientes configs, bothmes_us/Eread;35commonconfirmedhashes equal through2040,6replayedsteps,coopmagazines15->14 independently;host5/client0 normalshutdown.
- Parent fresh independent Spanishgameplay rerun completed exit0: `python -m uctest.language --case es`, report `build/autotest/language-1794489104814253100/report.json`. All actual Spanish opens/reads and gameplay passed again. Worker report's overly broad no_usarc statement is corrected: shared title_us is expected; no mes_us/E/dialog_us/game_us loaded.

## Limits
No live DE/FR/IT/JA or Linux/Deck/compatCPUsmoke yet; mapping/availability unitfixtures and nativeDOL evidence cover those contracts. det-out full1200retrace completion notrun (boundedtrace + Englishread proven). No lagspike duringmission (delayedrollbacktested atmenus then coopgameplay). Existing modifiedmes_us translations stay as-is, so English means resourcevariant, not rewritinglanguagecontents. Allinstalleddisc/saves remainuntouched.

## Review output
Return Verdict APPROVE / CHANGES_REQUIRED / CANNOT_VERIFY, Scope, Evidence (separate yoursourceobservations from parentcommands), Not verified, Findings + totals ifany. Read currentaffected/connectedsource; no shell or codechanges. Review current acceptance, not an assumedbranchdiff. Report any actualblockingdefect; don't substitute reviewfor missingcriticalproof.
