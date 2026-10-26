# Launcher Auto-update Implementation Plan

**Статус, 26 октября 2026:** этапы 1–8 реализованы и проверены локально.
Финальная `build.bat --no-install`: 197/197 тестов, 79.86 с; повторный E2E —
три успешных прогона. Чистый пакет: 213 файлов, ZIP 117385660 байт, проверен
нативным валидатором. Замечания итогового ревью исправлены и перепроверены.
Оба окна обновления доработаны по запросу пользователя, добавлена поддержка DPI.
Живая доставка публичного GitHub-релиза остаётся условной приёмкой после его
публикации; перенос между физическими мониторами и внешний ввод ОС не проверены.
Коммитов и отправки в GitHub не было. Подробный журнал —
`.superpowers/sdd/2026-10-25-launcher-auto-update/progress.md`.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Добавить ручной выпуск полного ZIP и автообновление Windows-клиента через публичные GitHub Releases с отдельным обновлятором и откатом.

**Architecture:** Общая библиотека проверяет версии, манифест и ZIP, планирует изменения и восстанавливает транзакцию. Лаунчер получает релиз в фоне и передаёт проверенный пакет отдельному процессу. Общая блокировка лаунчера, игры и обновлятора предотвращает замену используемых файлов.

**Tech Stack:** C++20, существующие SDL3/ImGui, nlohmann_json v3.12.0 и uc_miniz v3.0.2; Windows WinHTTP, BCrypt, Win32 process/file APIs; Python stdlib для выпуска; CMake/CTest и существующий clang-cl/Ninja build.

**Spec:** [Согласованный дизайн](../specs/2026-10-25-launcher-auto-update-design.md)

## Global Constraints

- Репозиторий: https://github.com/solleo95/reuc-recomp; публичный к выпуску; публикация вручную.
- build-release.bat → dist-release → UC-Windows-X.Y.Z.zip; корень ZIP содержит клиент.
- Один стабильный канал: latest release, без draft/prerelease; теги X.Y.Z или vX.Y.Z.
- Только подтверждённая пользователем установка; «Позже» действует до следующего запуска.
- HTTPS с проверкой сертификата; GitHub digest обязателен; ошибка сохраняет текущий клиент.
- Устанавливаются только файлы манифеста; неизвестные файлы сохраняются, коллизия останавливает установку.
- Сохраняются config.json, cpu-profile.json, saves/, mods/, textures/, game/, debug/, logs/.
- data/ содержит клиентские шейдеры/метаданные: разрешены только конкретные управляемые файлы.
- Без записи в реальную установленную игру/сохранения во время проверок. Любой uc.exe — только uctest, оконно.
- Без автоматического повышения привилегий, службы, дельта-загрузок, GitHub-токенов и новой ZIP-зависимости.
- Существующие изменения в launcher/, game/, common/, tests/ сохранить; никаких коммитов/пушей без отдельного запроса.
- Перед исполнением применить using-git-worktrees: выбрать checkout, содержащий текущие изменения; обычный новый worktree их не переносит.
- Не начинать исполнение до согласования плана и способа исполнения. Этот документ не разрешает публикацию.

## Review Focus

1. Прерывание между записью намерения и переименованием файла: восстановление повторяемо; проверки Task 4.
2. Junction/reparse point, изменение пути во время операции, различия регистра Windows: запись остаётся в проверенном корне; проверки Task 2/4.
3. Игра запущена напрямую или второй лаунчер стартует во время обновления: установка/запуск корректно блокируются; проверки Task 5.
4. Новый лаунчер требует DLL, ещё занятую helper, или пишет настройки до подтверждения старта: изолированный helper и ранний handshake; проверки Task 5/6.
5. ZIP уже опубликован локально, но переименование dist-release или финальная проверка упали: прежний релиз цел, новый результат не объявляется готовым; проверки Task 7.

---

## Карта файлов и порядок

Все пути ниже относительно корня репозитория. Строки существующих файлов — ориентиры на момент планирования; перед правкой сверить текущий код.

| Файлы | Ответственность |
| --- | --- |
| updater/release.h, release.cpp | Версии, release.json, GitHub metadata, правила состава |
| updater/package.h, package.cpp | SHA-256, безопасные пути и проверенная распаковка |
| updater/download.h, download.cpp | WinHTTP, таймауты, перенаправления, потоковая загрузка |
| updater/transaction.h, transaction.cpp | План изменений, backup, устойчивый журнал и восстановление |
| common/client_update_lock.h, client_update_lock.cpp | Межпроцессные блокировки жизненного цикла клиента |
| updater/process.h, process.cpp, main.cpp | Запуск helper, handshake, ожидание, перезапуск, окно состояния |
| launcher/update.h, update.cpp | UI-owned состояние, фоновая работа, модальное окно |
| scripts/build_release.py, build-release.bat | Проверенная сборка и чистая упаковка |
| updater/CMakeLists.txt | Общие библиотеки, helper и его установка |
| tests/test_update_*.cpp, test_build_release.py | Тесты формата, I/O, процессов и упаковки |
| docs/releasing.md | Выпуск, первичная установка и аварийное восстановление |

Изменяемые существующие файлы:
- CMakeLists.txt: добавить updater после modding и до tests; сохранить существующий порядок остального.
- common/CMakeLists.txt: небольшая Windows-библиотека блокировки.
- launcher/CMakeLists.txt, launcher/main.cpp, launcher/launch.cpp: подключение обновления и guard запуска.
- launcher/translations.inc: строки существующего механизма локализации.
- game/CMakeLists.txt, game/uc_main.cpp: ранняя блокировка прямого запуска игры.
- tests/CMakeLists.txt: CTest-регистрация по существующему образцу.
- .gitignore: только собственные временные release-результаты, если ещё не исключены.

build.bat и scripts/dev.bat переиспользуются. modding/mod_zip.* и mod_paths.* — образцы проверки, не повод менять моды: их cache/root semantics отличаются от установки клиента.

Порядок: 1 → 2 → 3 → 4 → 5 → 6 → 7 → 8. Task 3 и 4 зависят от общих контрактов, поэтому основной путь исполнения последовательный.

## Общие контракты

Пространство имён новых модулей: uc::update; существующие launcher/game API остаются в uc.
C++20: Result<T> = std::variant<T, Error>, Status = Result<std::monostate>.
Error содержит ErrorCode и диагностическую строку; UI выбирает локализованное сообщение по ErrorCode.
Набор кодов: invalid_version, invalid_manifest, invalid_release, unsafe_path, invalid_archive,
integrity, network, rate_limited, cancelled, busy, permission, no_space, io, startup, recovery_required.
Ожидаемое отсутствие новой версии выражается std::optional<Release>, не ошибкой.

Точный release.json format=1:
- version: каноническая X.Y.Z, компоненты 0..65535, без ведущих нулей.
- files: массив {path: UTF-8 relative forward-slash path, size: uint64, sha256: 64 lowercase hex}.
- helper_files: массив путей из files; включает uc_updater.exe и необходимые ему non-system DLL.
- release.json отсутствует в files; обновлятор обрабатывает его как управляемые метаданные.
- Обязательны uc.exe, uc_launcher.exe, uc_updater.exe. Остальные файлы определяет CMake install.
- helper_files разрешает только helper и root DLL, без путей наружу и без подмены запускаемого EXE.

Начальные технические лимиты (общие для packer и reader, проверяются на реальном выпуске):
- JSON GitHub: 1 MiB; release.json: 16 MiB; число entries: 50 000.
- ZIP: 2 GiB; распакованный пакет: 8 GiB; один файл: 2 GiB.
- WinHTTP: connect 5 s, send/receive inactivity 15 s; metadata overall 30 s,
  download overall 30 min; максимум 5 redirects.
- Максимальное ожидание закрытия лаунчера/блокировки: 30 s; startup acknowledgement: 30 s.
- Служебный корень: .uc-update/; операция: случайный GUID. Никаких пользовательских путей в CLI удаления.
- Упаковщик отказывает при превышении лимитов. Если штатный пакет их превышает,
  скорректировать обе стороны по измеренному размеру до выпуска, задокументировать.

### Task 1: Формат релиза, версии и политика файлов

**Files:** Create updater/release.h, updater/release.cpp, updater/CMakeLists.txt,
tests/test_update_release.cpp, tests/fixtures/update/release-v1.json;
Modify CMakeLists.txt, tests/CMakeLists.txt.

**Interfaces:**
- Version {uint16_t major, minor, patch}; сравнение по компонентам.
- FileEntry {std::string path; uint64_t size; std::string sha256}.
- Manifest {Version version; std::vector<FileEntry> files; std::vector<std::string> helper_files}.
- Release {Version version; std::string asset_url, notes, sha256; uint64_t size}.
- Result<Version> parse_version(std::string_view text, bool allow_v_prefix).
- Result<Manifest> parse_manifest(std::string_view json).
- Result<std::optional<Release>> select_release(std::string_view json, Version installed).
- Status validate_managed_path(std::string_view relative).
- std::string format_version(Version version).

- [ ] **1. RED:** создать test_update_release с assertions ниже, зарегистрировать CTest test_update_release и минимальный компилируемый uc_update_core.

```cpp
CHECK(ok(parse_version("0.0.9", false)) < ok(parse_version("0.0.10", false)));
CHECK(ok(parse_version("v0.0.1", true)) == ok(parse_version("0.0.1", false)));
CHECK(is_error(parse_version("01.0.0", false)));
CHECK(is_error(parse_version("65536.0.0", false)));
CHECK(is_error(validate_managed_path("cpu-profile.json")));
CHECK(is_error(validate_managed_path("GAME/DATA/sys/main.dol")));
CHECK(is_ok(validate_managed_path("data/shaders/example.spv")));
```

CHECK/ok/is_error — локальные test helpers по образцу существующих assert-тестов, без нового test framework.
Добавить malformed JSON, wrong types, negative/overflow sizes, duplicate paths, bad hashes,
self-entry release.json, missing helper/required EXEs, tag/asset mismatch, old/equal version,
draft/prerelease, missing/duplicate ZIP asset и digest. Пример корректного asset: UC-Windows-0.0.1.zip.

- [ ] **2. RED run:** cmake -S . -B build; cmake --build build --target test_update_release; ctest --test-dir build -R "^test_update_release$" --output-on-failure. Ожидание: тест падает на незавершённом поведении.
- [ ] **3. GREEN:** реализовать контракты через существующий nlohmann_json, без сетевого I/O; отклонять неизвестный format и небезопасные пути с Windows-нормализацией.
- [ ] **4. GREEN run:** повторить target/CTest; PASS. Включить таблицу защищённых областей, подтверждённую текущим кодом записи настроек/профилей/логов.
  release-v1.json — общий минимальный корректный образец для C++ reader и Python packer;
  содержит три обязательных EXE, их реальные fixture hashes и helper_files.
- [ ] **5. Проверка diff:** только формат, политика и их тесты; не коммитить.

### Task 2: Проверка ZIP и безопасный staging

**Files:** Create updater/package.h, updater/package.cpp, tests/test_update_package.cpp;
Modify updater/CMakeLists.txt, tests/CMakeLists.txt.

**Interfaces:**
- Uses Manifest/Release/Status from Task 1.
- Package {std::filesystem::path root; Manifest manifest}.
- Result<std::string> sha256_file(const std::filesystem::path& file).
- Result<Package> prepare_package(const std::filesystem::path& zip,
  const Release& release, const std::filesystem::path& staging, std::stop_token stop).
- Status verify_package(const Package& package).
- Status validate_target_path(const std::filesystem::path& install,
  std::string_view relative).
- test_update_package также принимает --verify-release <zip> <version> <sha256> <size>:
  вызывает тот же prepare_package во временном каталоге, возвращает 0 при успехе, 1 при ошибке.
  Этот build-time validator не входит в релиз и нужен Task 7.

- [ ] **1. RED:** test_update_package строит маленькие ZIP через uc_miniz; сценарии assertions:

```cpp
CHECK(is_error(prepare("traversal.zip")));     // ../outside.txt; outside unchanged
CHECK(is_error(prepare("case-collision.zip"))); // a.dll + A.dll
CHECK(is_error(prepare("protected-config.zip")));
CHECK(is_error(prepare("wrong-digest.zip")));
CHECK(is_error(prepare("symlink.zip")));
CHECK(read(outside) == before);
CHECK(is_ok(prepare("valid-client.zip")));
```

prepare — локальный fixture helper к prepare_package. Проверить также UNC/drive/ADS, trailing
dot/space, DOS device names, NUL/invalid UTF-8, file-directory collisions, длинные имена,
encrypted/unsupported entry, CRC failure, размер/количество, extra/missing entry, cancellation,
root/parent junction и destination junction. Длинные имена не должны молча обрезаться.

- [ ] **2. RED run:** build target test_update_package; CTest -R "^test_update_package$"; ожидается FAIL.
- [ ] **3. GREEN:** линковать существующий uc_miniz и системный bcrypt. Проверить digest до распаковки,
  перечислить полный состав до записи; имена получать через mz_zip_reader_get_filename,
  не усечённый m_filename. Извлекать потоково с контролем фактических байтов, хешей и границ.
  Не передавать непроверенные имена API, которое само создаёт пути.
- [ ] **4. GREEN:** использовать Unicode Win32 paths/handles; не следовать reparse points.
  Проверять destination при использовании, а не только предварительно; удерживать необходимые
  handles, чтобы проверенный parent нельзя было подменить между проверкой и записью.
  Проверить staged files перед передачей helper и снова в helper перед применением.
- [ ] **5. GREEN run:** target/CTest PASS; при ошибке удаляется только staging этой операции,
  рабочая установка и посторонние temp-файлы неизменны. Junction-проверки обязательны на Windows.

### Task 3: GitHub transport и отменяемая загрузка

**Files:** Create updater/download.h, updater/download.cpp, tests/test_update_download.cpp;
Modify updater/CMakeLists.txt, tests/CMakeLists.txt.

**Interfaces:**
- HttpResponse {unsigned status; std::string body}; метаданные в пределах 1 MiB.
- Transport — узкий интерфейс get_metadata(std::stop_token) -> Result<HttpResponse>,
  download(const Release&, const std::filesystem::path&, Progress, std::stop_token) -> Status.
- Progress = std::function<void(uint64_t downloaded, uint64_t total)>.
- WinHttpTransport implements Transport с фиксированным repository endpoint.
- Result<std::optional<Release>> check_release(Transport&, Version, std::stop_token).
- Status download_release(Transport&, const Release&, const std::filesystem::path&,
  Progress, std::stop_token).

- [ ] **1. RED:** test_update_download с fake Transport проверяет 404/403/429/5xx, malformed/oversize
  metadata, digest absent, stalled/short/oversize download, hash mismatch, cancellation:

```cpp
CHECK(check(fake_404).error == ErrorCode::network);
CHECK(download(fake_bad_digest).error == ErrorCode::integrity);
CHECK(download(fake_cancelled).error == ErrorCode::cancelled);
CHECK(!exists(final_zip_after_failure));
```

Добавить unit cases URL policy: HTTPS github.com/solleo95/reuc-recomp/releases/download/
для исходного вложения; redirects только github.com, release-assets.githubusercontent.com
и objects.githubusercontent.com, без credentials, нестандартных портов и downgrade.
Фактические GitHub redirects проверить до выпуска; расширение списка — только обоснованное точное имя.

- [ ] **2. RED run:** target test_update_download и одноимённый CTest; ожидается FAIL.
- [ ] **3. GREEN:** WinHTTP на worker thread, стандартная TLS-проверка, User-Agent, ручной контроль
  redirect policy/лимитов. Запись .part; переименование после размера/SHA-256. Отмена между
  блоками, ограниченные таймауты; ownership WinHTTP handles исключает close/use race.
- [ ] **4. GREEN run:** все fake tests PASS; проверить, что diagnostic не содержит redirect query
  с временной подписью. Unit tests не зависят от GitHub и не требуют смены production endpoint.
- [ ] **5. Сборка Windows transport:** target uc_update_download; ошибки сети возвращаются
  типизированно, никогда не выключают обычный запуск клиента.

### Task 4: Транзакция установки и восстановление

**Files:** Create updater/transaction.h, updater/transaction.cpp,
tests/test_update_transaction.cpp; Modify updater/CMakeLists.txt, tests/CMakeLists.txt.

**Interfaces:**
- InstallPlan {install_root, operation_root, old_manifest, new_manifest, operations};
  операции: replace, add, remove, metadata; пути получены только после валидации.
- Result<InstallPlan> plan_install(const std::filesystem::path& install, const Package& package).
- Status apply_install(const InstallPlan& plan).
- Status finish_install(const std::filesystem::path& operation).
- Status recover_install(const std::filesystem::path& install, const std::filesystem::path& operation).
- Журнал format=1: operation_id, normalized install identity, old/new versions,
  phase (prepared/applying/awaiting_startup/committed/rolling_back/rolled_back),
  per-file intent/completion, prior existence, backup path и ожидаемые hashes.
- Fault injection — внутренняя test seam на I/O checkpoints; production CLI её не принимает.

- [ ] **1. RED:** test_update_transaction на temp-копии фиктивной установки:

```cpp
CHECK(apply_then_recover(fixture) == fixture.original_tree());
CHECK(read(fixture.path("saves/user.dat")) == "USER");
CHECK(read(fixture.path("mods/local.lua")) == "USER");
CHECK(plan_with_unknown_file_collision().error == ErrorCode::unsafe_path);
CHECK(recover_twice_after_crash() == original_tree);
```

Добавить add/replace/remove и metadata rollback, поддельный старый манифест с protected path,
отсутствующий старый файл, directory collision, junction swap, disk full, permission denied,
locked destination, failure during rollback. Проверить отсутствие изменений вне root.

- [ ] **2. RED run:** target test_update_transaction; одноимённый CTest FAIL.
- [ ] **3. GREEN:** полный preflight свободного места/прав/конфликтов до первой мутации.
  backup на том же томе; journal write + FlushFileBuffers до каждой мутации.
  Переименования и journal state позволяют отличать pending шаг от выполненного по
  существованию и проверенным hashes. Не доверять путям из локального журнала.
- [ ] **4. GREEN:** новый release.json последним; commit только после startup ack.
  recover_install идемпотентен для каждого pending/done состояния, сохраняет backup при ошибке.
  При повреждённом/неполном журнале fail closed и понятная диагностика, без угадывания путей.
- [ ] **5. Crash tests:** дочерний test-процесс принудительно прерывается на каждом checkpoint
  до/после файловой операции. Следующий test-процесс восстанавливает исходное дерево;
  committed-транзакция сохраняет новый клиент и только завершает cleanup.
- [ ] **6. GREEN run:** target/CTest PASS; журнал не очищается, пока commit/rollback не подтверждены.

### Task 5: Отдельный обновлятор и межпроцессная координация

**Files:** Create common/client_update_lock.h, common/client_update_lock.cpp,
updater/process.h, updater/process.cpp, updater/main.cpp,
tests/test_update_process.cpp, tests/update_test_child.cpp;
Modify common/CMakeLists.txt, updater/CMakeLists.txt, tests/CMakeLists.txt,
game/CMakeLists.txt, game/uc_main.cpp.

**Interfaces:**
- uc::ClientSessionGuard — move-only RAII shared usage lock;
  std::variant<ClientSessionGuard, std::error_code> enter_client_session(
  const std::filesystem::path& install_root, const std::optional<ResumeTicket>& ticket).
- uc::ClientInstallGuard — move-only RAII gate + exclusive replacement lock;
  std::variant<ClientInstallGuard, std::error_code> reserve_client_install(
  const std::filesystem::path& install_root);
  wait_until_unused(timeout), allow_child_start(), reacquire_for_rollback(timeout).
  ResumeTicket — private IPC identity + operation ID, валидируемые при входе.
  common не зависит от updater; коды Windows отображаются в update::Error на границе.
- Result<HelperProcess> start_update_helper(install_root, Package package);
  HelperProcess owns process handle + startup channel; ready() only after preflight.
- Status run_update_helper(operation_root, parent_process_handle).
- Status resume_pending_update(install_root).
- Helper CLI: --apply <operation-dir> или --recover <operation-dir>.
  По запуску без аргументов служебная копия предлагает восстановить свою операцию.
- Контекст операции содержит нормализованный root и ID; внешние команды/EXE-пути не принимаются.

- [ ] **1. RED:** test_update_process + update_test_child проверяют два лаунчера/два installer,
  shared session занят, parent не завершился, helper не запустился, ready timeout,
  неправильный token, child exits-before-ack, child succeeds и recovery после killed helper.
  В тестах child — fixture EXE; настоящий uc.exe не запускать.
- [ ] **2. RED run:** targets test_update_process, update_test_child; CTest FAIL.
- [ ] **3. GREEN — locks:** короткий per-install named gate сериализует вход в сессию;
  shared byte-range lock файла .uc-update/session.lock живёт всю сессию.
  Installer держит gate, разрешает исходному лаунчеру закрыться, ждёт exclusive file lock.
  Уже работающая игра/другой лаунчер остаются целы; timeout до замены вызывает понятный отказ.
  Новые обычные старты в этот момент сообщают об обновлении и завершаются.
- [ ] **4. GREEN — helper:** CreateProcessW с явным application path, Unicode quoting,
  ограниченный список inherited handles; parent handle с ожиданием, а не голый PID.
  Ready и startup ack через приватный IPC с operation ID и одноразовым resume ticket.
  На запуске новой версии exclusive usage lock освобождается, gate остаётся у helper;
  только подтверждённый child обходит gate и получает shared session lock.
  При rollback сначала завершить только созданный child, затем вернуть exclusive lock.
- [ ] **5. GREEN — bootstrap:** ранний guard в game/uc_main.cpp:wmain, до работы с ресурсами.
  Helper копируется в .uc-update/<GUID>/recovery/ вместе с helper_files; его зависимости
  берутся из текущего установленного манифеста: запускается текущий helper для установки
  нового пакета. Неподдерживаемый формат нового пакета останавливает операцию до закрытия UI.
  Все скопированные helper-файлы сверяются с текущим манифестом. Зависимости
  должны загружаться из recovery или Windows system directories. Не предполагать /MT:
  проверить PE imports, включить необходимую runtime closure в install metadata.
  Helper не линкуется с SDL/ImGui/game и не держит заменяемые DLL из install root.
- [ ] **6. GREEN — lifecycle:** verify_package ещё раз, apply_install, запуск строго
  install_root/uc_launcher.exe с cwd=install_root, ожидание ack 30 s; finish_install
  либо recover_install и запуск старого лаунчера. Native Win32 progress/error window,
  без консоли и повышения прав. Ошибка recovery оставляет backup и показывает путь.
- [ ] **7. GREEN run:** CTest PASS, отдельно проверить default-launch recovery и отсутствие
  записи пользовательских настроек test-child до разрешения handshake.
  Зарегистрировать uc_updater.exe и runtime closure в CMake install.

### Task 6: Интеграция лаунчера и модальное окно

**Files:** Create launcher/update.h, launcher/update.cpp, tests/test_update_controller.cpp;
Modify launcher/main.cpp, launcher/launch.cpp, launcher/CMakeLists.txt,
launcher/translations.inc, tests/CMakeLists.txt.

**Interfaces:**
- UpdateController owns worker, stop_source, operation generation и UI state.
- State: disabled, checking, available, downloading, verifying, ready_to_close, failed.
- start(Manifest installed), accept(), postpone(), cancel(), poll(),
  bool blocks_game_launch() const, bool close_requested() const.
- draw_update_modal(UpdateController&) использует существующие style/translations.
- Uses Task 3 Transport и Task 5 process APIs; тесты подставляют fake Transport/helper.
- Любой normal/dev startup вызывает recovery check/guard до чтения/записи настроек;
  auto-check включается только при валидном release.json.

- [ ] **1. RED:** test_update_controller проверяет startup single-flight, старый результат
  после cancel/retry, double-click Update, отсутствие сети, dev install, running game,
  helper failure before ready, shutdown while downloading и restore Play after error:

```cpp
CHECK(fake.download_calls == 1);       // double accept
CHECK(!controller.close_requested());  // helper failed before ready
CHECK(!controller.blocks_game_launch()); // cancel/error recovered
CHECK(stale_completion_did_not_change_state);
```

- [ ] **2. RED run:** target test_update_controller и CTest FAIL.
- [ ] **3. GREEN — lifecycle:** подключить early recovery/session guard в launcher/main.cpp
  до config_path/cpu-profile обработки; проверять launch_game повторно в launcher/launch.cpp.
  Новый лаунчер признаёт bootstrap ticket; подтверждает минимальную инициализацию после
  готовности UI/ресурсов, до записи config и без ожидания HTTP. Обработка pending operation
  отличает собственный trial startup от обычного восстановления.
  После ack child ждёт подтверждения committed от helper; до него запрещены запись
  настроек и Play. Ошибка фиксации не оставляет настройки от неуспешной новой версии.
- [ ] **4. GREEN — threading:** UI-thread owns состояние; worker передаёт immutable result,
  progress синхронизирован; устаревший generation игнорируется, shutdown отменяет/joins worker.
  Состояние ready_to_close только после helper ready, then обычный путь завершения.
- [ ] **5. GREEN — UI:** native ImGui modal по существующему BeginPopupModal pattern:
  версии, plain-text notes, размер, «Обновить»/«Позже», progress/«Отмена», error/retry.
  Добавить переводы всех затронутых строк в существующие языки; сохранить стиль/fonts.
  При запущенной игре объяснить необходимость закрытия, без принудительного завершения.
- [ ] **6. GREEN run:** CTest PASS; собрать uc_launcher и uc.
  Через native UI на fixture install проверить клики, отмену, повтор и закрытие.
  Chrome MCP не управляет ImGui: применять доступный native computer-use;
  при отсутствии перечислить непроверенные UI-сценарии, не заменять их одним screenshot.

### Task 7: Чистая релизная сборка и ZIP

**Files:** Create build-release.bat, scripts/build_release.py, tests/test_build_release.py,
docs/releasing.md; Modify tests/CMakeLists.txt, .gitignore (если требуется).

**Interfaces:**
- Python: parse_version(text: str) -> str; write_manifest(stage: Path) -> dict;
  package_release(stage: Path, version: str, output: Path) -> Path;
  build_release(repo: Path, version: str) -> Path; main() -> int.
- build-release.bat: перейти в свой каталог, вызвать Python script, сохранить exit code.
  Вопрос «Версия релиза (например, 0.0.1):» задаёт Python, чтобы сырой ввод не попадал в shell.
- build_release использует существующий build.bat --no-install; только после успешного exit
  повторный cmake --install build --prefix <own-clean-stage>.
- Helper runtime metadata берётся из CMake install Task 5; фактический helper_files входит в release.json.

- [ ] **1. RED:** tests/test_build_release.py на unittest/tempdir, fake build/install runner:

```python
self.assertEqual(parse_version("0.0.1"), "0.0.1")
with self.assertRaises(ValueError):
    parse_version("0.0.1 & whoami")
self.assertEqual(zip_path.name, "UC-Windows-0.0.1.zip")
self.assertNotIn("saves/user.dat", zip_names)
self.assertNotIn("uc_experiment.exe", zip_names)
self.assertEqual(installed_user_save.read_bytes(), b"USER")
```

Добавить build/test/install failure, pre-existing same-version ZIP, previous dist-release,
partial publication failure, Unicode/spaces, unexpected symlink, polluted dist, missing
mandatory runtime/metadata, helper closure и соответствие C++ manifest/ZIP validator.

- [ ] **2. RED run:** python -I tests/test_build_release.py .; ожидается FAIL. Зарегистрировать
  test_build_release в CTest через существующий Python3_EXECUTABLE pattern.
- [ ] **3. GREEN:** версия валидируется до запуска build. Для этой .bat-цепочки использовать
  штатный Windows batch запуск; никакой подстановки пользовательского ввода в командный текст.
  Временный каталог рядом с результатом, проверенный owner marker и confinement перед cleanup.
  Нельзя брать файлы из текущего dist или установленной игры.
- [ ] **4. GREEN:** manifest/hashlib/zipfile из Python stdlib; стабильная сортировка files,
  root ZIP layout, обязательные runtime-файлы из install manifest.
  Запуск build/tests/test_update_package.exe --verify-release <zip> <version> <sha256> <size>
  из Task 2 на итоговом архиве до публикации; путь target сверить с CMake runtime output.
  Политику/лимиты держать согласованными через shared fixtures, не вводить второй формат.
- [ ] **5. GREEN — publication:** существующий ZIP той же версии → понятный отказ.
  Новый dist-release заменять через backup/rename только после проверки ZIP; временный ZIP
  переименовывать в UC-Windows-X.Y.Z.zip последним. При ошибке восстанавливать предыдущий
  dist-release, не оставлять финальный ZIP. Cleanup не трогает чужие каталоги/файлы.
- [ ] **6. GREEN run:** Python suite + CTest PASS; в docs/releasing.md описать version/tag/Latest,
  единственный ZIP asset, ручную первую установку, protected paths, recovery и место логов.
  Указать: release.json/managed files при обновлении защищают пользовательские данные,
  но ручная распаковка старого клиента не выполняет алгоритм обновлятора.

### Task 8: Сквозная проверка и завершение

**Files:** Create tests/test_update_end_to_end.py; Modify tests/CMakeLists.txt,
docs/releasing.md (только подтверждённые инструкции).

CTest name: test_update_end_to_end; Python3_EXECUTABLE -I запускает script с путями
к updater/test-child targets через CMake generator expressions, без догадок об output directory.

**Interfaces:** Uses package builder Task 7, updater EXE Task 5, update_test_child fixtures;
никаких production-only backdoor flags или HTTP endpoint override.

- [ ] **1. RED:** сценарий A=0.0.1 → B=0.0.2 на fixture install: один файл заменён, один добавлен,
  один удалён; сохранения/настройки/моды/неизвестные файлы совпадают побайтно.
  Stub launcher подтверждает startup через штатный IPC; следующая проверка видит B.
- [ ] **2. RED:** B без startup ack → восстановлен A; killed helper → повторный recover возвращает A;
  второй recover ничего не портит; committed journal после crash сохраняет B.
  Следующий релиз обновляет и uc_updater.exe, и его DLL без блокировки исполняемой recovery-копии.
- [ ] **3. GREEN run:** собрать целевые fixtures и выполнить CTest -R "test_update_|test_build_release".
  Все PASS; тесты не запускают uc.exe и не обращаются к реальным saves/install.
- [ ] **4. Full verification:** .\build.bat --no-install; при успехе штатные build/tests/staging завершены.
  Не повторять полный suite без новой причины. Отдельно измерить чистый релиз и проверить лимиты.
- [ ] **5. Native acceptance:** изолированная копия реального лаунчера с fixture transport в test
  build проходит modal/download/cancel/helper/restart/error; production binary не получает
  публичного способа переключить доверенный источник обновлений.
- [ ] **6. Live GitHub read-only acceptance:** после появления публичного релиза проверить latest,
  asset name, digest и фактические HTTPS redirect hosts. До этого зафиксировать ограничение
  «публичный GitHub выпуск ещё не проверен»; ничего не загружать/публиковать от имени пользователя.
- [ ] **7. Review:** после завершения плана — финальный review всего diff по правилам проекта.
  Если работаем по большим этапам, cavecrew-reviewer один раз на завершённый этап, не после
  каждого test command; финальный whole-branch reviewer — gpt-6-astra. Исправления перепроверить
  соответствующим тестом. Коммиты/пуши только по отдельному запросу.
- [ ] **8. Handoff:** указать изменённые компоненты, выполненные проверки, запуск build-release.bat,
  ручные действия публикации и оставшиеся ограничения; вернуть пользователю рабочие файлы.

## Самопроверка плана

- Покрытие дизайна: packaging → Task 7; поиск/UI → 3/6; ZIP/paths → 1/2;
  ownership → 1/4; helper/rollback/recovery → 4/5; verification → 8.
- Все пять Review Focus имеют конкретные тесты в указанных задачах.
- Общие типы задаёт Task 1; Package — Task 2; transport — 3; journal — 4; IPC/guards — 5.
- Метаданные helper_files закрывают отсутствие гарантии static CRT; data/ не заблокирован целиком.
- Вызовы сборки/тестов выполняются при исполнении; этот план не утверждает, что они уже прошли.
- Рабочие изменения сохраняются; в этом сеансе планирования продуктовый код не меняется.

## Источники решений по зависимостям

- Существующие target/dependency declarations: common/CMakeLists.txt и modding/CMakeLists.txt.
- [miniz 3.0.2](https://github.com/richgel999/miniz/tree/3.0.2): существующая ZIP-библиотека.
- [miniz reader API](https://github.com/richgel999/miniz/blob/master/_autodocs/api-reference/zip-reader.md):
  полные имена получать отдельно от ограниченного file_stat буфера; при реализации сверить с pinned headers.
- [GitHub Releases REST](https://docs.github.com/en/rest/releases/releases):
  стабильный latest release и метаданные вложения/digest.

