# Linux: Windows-вид вне Game Mode — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** на Linux вне Game Mode `uc_launcher` и меню в игре выглядят как на Windows; в Game Mode остаётся Deck-вид; вид можно форсировать `--ui desktop|deck`.

**Architecture:** маленькая общая функция выбирает вид (`--ui`, затем `UC_UI`, затем `GAMESCOPE_WAYLAND_DISPLAY`). Игра создаёт Deck-меню только при виде `deck`. Windows-лаунчер (`launcher/main.cpp`) собирается и на Linux через тонкий слой платформы; Deck-лаунчер остаётся как есть и превращается в функцию. Один бинарник `uc_launcher` на Linux содержит оба вида.

**Tech Stack:** C++20, SDL3, ImGui, CMake (Windows: clang-cl; Linux: кросс-сборка `scripts/linux/build.ps1`), тесты `check.h`, harness `uctest` на Deck по ssh.

**Spec:** `docs/superpowers/specs/2026-11-02-linux-desktop-ui-design.md`

## Global Constraints

- Game Mode = задана `GAMESCOPE_WAYLAND_DISPLAY` (непустая); `SteamGameId` в условие не входит.
- Приоритет вида: `--ui` > `UC_UI` > автоопределение; значения `desktop` и `deck`; любое другое значение флага или `UC_UI` = ошибка и выход, без молчаливого выбора.
- Существующие проверки gamescope в `main_deck.cpp`, `compile_screen_posix.cpp`, `uc_main.cpp` (HDR), `steam_overlay.cpp` не менять.
- Десктопный вид на Linux показывает то же, что Windows-вид, минус: DX11/DX12, DLSS/Streamline, CPU-калибровка, DLSS-панель, вкладка сетевой игры. Deck-only элементы (кнопка гироскопа, Recenter aim, Quit game, выбор пути в стиле Deck) в нём не появляются. Скрывать, а не показывать отключённым.
- Лаунчер запускает игру как на Windows (процесс), не `exec`, и передаёт ей `--ui <вид>`.
- Один `config.json` на оба вида; значения Game Mode (HDR 400/300 нит) применяются при запуске и не должны перезаписываться десктопным видом.
- Поведение Windows не меняется: после каждой задачи проходят все тесты Windows (`ctest --test-dir build`).
- Проверять кадры только игровыми снимками и снимком самого лаунчера, не рабочий стол пользователя.
- Коммиты только по просьбе владельца; шаги «Commit» ниже выполнять, только если он попросит.

## Review Focus

1. `UC_UI` или `--ui` пустая строка или с большими буквами: ошибка, а не молчаливый выбор (T1).
2. `GAMESCOPE_WAYLAND_DISPLAY` задана пустой строкой: считается Desktop (T1).
3. Принудительный `--ui deck` на Desktop без геймпада: Deck-вид запускается и закрывается мышью/Esc без падения (T7).
4. Принудительный `--ui desktop` в Game Mode: окно лаунчера помещается на экране 1280×800 и управляется мышью/касанием (T7).
5. Десктопный вид не затирает Game Mode значения HDR в `config.json` (T6).

---

### Task 1: Выбор вида (`uc::resolve_ui_mode`)

**Files:**
- Create: `common/ui_mode.h`, `common/ui_mode.cpp`, `tests/test_ui_mode.cpp`
- Modify: `common/CMakeLists.txt` (библиотека `uc_ui_mode`), `tests/CMakeLists.txt` (добавить `test_ui_mode` в тот же список, где `test_options`)

**Interfaces:**
- Produces:
  ```cpp
  namespace uc {
  enum class UiMode { desktop, deck };
  std::string_view ui_mode_name(UiMode);   // "desktop" | "deck"
  // flag: value of --ui, env_ui: UC_UI, gamescope: GAMESCOPE_WAYLAND_DISPLAY (nullptr = unset)
  std::variant<UiMode, std::string> resolve_ui_mode(std::optional<std::string_view> flag, const char* env_ui, const char* gamescope);
  }
  ```

- [ ] **Step 1: Write the failing test** `tests/test_ui_mode.cpp`: проверки `resolve_ui_mode({}, nullptr, nullptr)==desktop`; `({}, nullptr, "gamescope-0")==deck`; `({}, nullptr, "")==desktop`; `({"desktop"}, nullptr, "gamescope-0")==desktop`; `({"deck"}, nullptr, nullptr)==deck`; `({}, "deck", nullptr)==deck`; `({"desktop"}, "deck", nullptr)==desktop` (флаг сильнее env); `({}, "desktop", "gamescope-0")==desktop`; ошибка (variant хранит string) для `({"Deck"},…)`, `({""},…)`, `({}, "x", nullptr)`, `({}, "", nullptr)`; `ui_mode_name`.
- [ ] **Step 2: Run** `cmake --build build --target test_ui_mode` — ожидается ошибка сборки (нет заголовка).
- [ ] **Step 3: Implement** функции в `common/ui_mode.cpp` по таблице из теста; сообщение ошибки содержит неверное значение.
- [ ] **Step 4: Run** `ctest --test-dir build -R test_ui_mode --output-on-failure` — PASS.
- [ ] **Step 5: Commit** (только по просьбе).

### Task 2: `--ui` в параметрах игры и условие Deck-меню

**Files:**
- Modify: `game/options.h` (`std::optional<std::string> ui;` в `Options`), `game/options.cpp` (разбор `--ui`, образец: `--renderer`), `game/uc_main.cpp` (создание `deck_menu` вокруг строки ~1031 и `[ui]`-лог), `game/CMakeLists.txt` (линковка `uc_ui_mode`)
- Test: `tests/test_options.cpp`

**Interfaces:**
- Consumes: `uc::resolve_ui_mode` (T1)
- Produces: `Options::ui` (значение `--ui` как строка); в игре на Linux при старте печатается одна строка `[ui] mode <desktop|deck> (<flag|env|gamescope|default>)`.

- [ ] **Step 1: Failing test** в `tests/test_options.cpp`: `parse({"--ui","deck"})` даёт `Options` с `ui=="deck"`; `{"--ui"}` без значения и `{"--ui",""}` дают строку ошибки; без аргумента `ui` пусто.
- [ ] **Step 2: Run** `ctest -R test_options` — FAIL.
- [ ] **Step 3: Implement** разбор в `options.cpp` (значение проверяется позже функцией T1, здесь только сохраняется).
- [ ] **Step 4: Implement** в `uc_main.cpp`: на не-Windows вычислить `UiMode` через `resolve_ui_mode(options.ui, getenv("UC_UI"), getenv("GAMESCOPE_WAYLAND_DISPLAY"))` до создания меню; при ошибке напечатать её и выйти с кодом 2; `deck_menu.emplace(...)` только при `deck`; напечатать строку `[ui] mode …`. Все остальные места, использующие `deck_menu`, уже проверяют указатель.
- [ ] **Step 5: Run** `ctest -R "test_options|test_ui_mode"` PASS; собрать Linux `pwsh scripts/linux/build.ps1 -Target uc`.
- [ ] **Step 6: Verify on Deck (Desktop Mode)** сценарий `pause` из `scripts/uctest/scenarios.py` с `--deck --game-arg=--ui --game-arg=desktop`: в логе `[ui] mode desktop (flag)`, снимок `menu` показывает Windows-меню; затем с `--ui deck`: Deck-меню.
- [ ] **Step 7: Commit** (только по просьбе).

### Task 3: Слой платформы лаунчера и запуск игры на POSIX

**Files:**
- Create: `launcher/platform.h`, `launcher/platform_win.cpp`, `launcher/platform_posix.cpp`
- Modify: `launcher/main.cpp` (вызовы заменяют код с `windows.h`/DXGI/DWM/`GetModuleFileNameW`), `launcher/launch_posix.cpp` (добавить `launch_game`, `game_running`, `ProcessCloser`), `launcher/CMakeLists.txt`
- Test: `tests/test_launch_posix.cpp` (только Linux, `tests/linux/CMakeLists.txt`)

**Interfaces:**
- Produces (`launcher/platform.h`, namespace `uc::platform`): `std::filesystem::path executable_dir();` `std::string gpu_name(SDL_Window*);` (пусто, если неизвестно) `void style_window(SDL_Window*);` (тёмный заголовок на Windows, на Linux пусто) `void open_folder(const std::filesystem::path&);`
- Linux: `uc::launch_game(...)` с сигнатурой из `launcher/launch.h`, запуск `posix_spawn` с аргументами конфигурации так же, как Windows; `ProcessCloser` ждёт/закрывает дескриптор процесса.

- [ ] **Step 1: Failing test** `tests/test_launch_posix.cpp`: `launch_game` запускает `/bin/true` (путь передаётся как «исполняемый файл»), `game_running` сперва true/false по состоянию, после ожидания false; несуществующий файл бросает `std::runtime_error`.
- [ ] **Step 2: Run** на Deck (`scripts/linux/deck.py run`) — FAIL (нет символов).
- [ ] **Step 3: Implement** `launch_game`/`game_running`/`ProcessCloser` в `launch_posix.cpp`.
- [ ] **Step 4: Implement** `platform.h` + две реализации; перенести в `platform_win.cpp` существующий Windows-код из `main.cpp` без изменения поведения; `main.cpp` вызывает `uc::platform::…`.
- [ ] **Step 5: Run Windows** `cmake --build build` и `ctest --test-dir build -R "launcher|update"` — PASS (Windows-лаунчер не изменился).
- [ ] **Step 6: Commit** (только по просьбе).

### Task 4: Windows-лаунчер собирается на Linux

**Files:**
- Modify: `launcher/main.cpp`, `launcher/CMakeLists.txt` (Linux-ветка: цель `uc_desktop_launcher`, STATIC, функция входа `int run_desktop_launcher(int, char**)`), `launcher/ui_fonts.cpp`, `launcher/graphite*.cpp` при необходимости, `launcher/settings_registry.*` (Task 5)
- Create: `launcher/desktop_entry.h`

**Interfaces:**
- Produces: `int uc::launcher::run_desktop_launcher(int argc, char** argv);` (Linux; на Windows остаётся `wWinMain`).

- [ ] **Step 1: Configure** Linux-ветку: `uc_desktop_launcher` из `main.cpp`, `graphite.cpp` (Linux-часть через `graphite_posix.cpp`), `graphite_art.cpp`, с библиотеками, которые уже собираются на Linux (`uc_binding_editor`, `uc_settings_registry`, `uc_gamepad_panel`, `uc_gamepad_input`, `uc_wiimote_input`, `uc_launch_probe`, `uc_image_unpack`, `uc_imgui`, `uc_ui_style`, `uc_prompt_icons`, `uc_launcher_locale`, `uc_mod_menu`, `uc_update_ui`). Исключить `uc_nat`, `uc_cpu_calibration`, `uc_launch` (Windows); их использование в `main.cpp` закрыть `#ifdef _WIN32`.
- [ ] **Step 2: Build** `pwsh scripts/linux/build.ps1 -Target uc_desktop_launcher`; ожидаются ошибки только в `main.cpp` (`wchar_t`/`std::wstring` в путях и аргументах, `windows.h`, сетевая вкладка, DLSS-панель, калибровка CPU). Устранять по классам: пути и аргументы через `std::filesystem::path` и `char**`; Windows-специфику за `#ifdef _WIN32` или через `uc::platform`.
- [ ] **Step 3: Implement** `run_desktop_launcher(int, char**)` из тела `run(...)`/`wWinMain` без Windows-обвязки (обновление через `update_posix.cpp`, как у Deck).
- [ ] **Step 4: Verify** сборка Linux проходит без предупреждений об неопределённых символах; Windows `ctest --test-dir build` PASS.
- [ ] **Step 5: Commit** (только по просьбе).

### Task 5: Скрытие Windows-только настроек на Linux

**Files:**
- Modify: `launcher/settings_registry.h/.cpp` (признак платформы: `bool available_on_this_platform(std::string_view id)`), `launcher/main.cpp` (вкладка сети, DLSS-панель, калибровка CPU, пункты DX11/DX12 в списке рендерера)
- Test: `tests/test_settings_registry.cpp`

**Interfaces:**
- Produces: `uc::settings::available_on_this_platform(std::string_view id)`; `shown(...)` возвращает `false` для недоступных.

- [ ] **Step 1: Failing test**: функция `uc::settings::hidden_on_linux()` (список id) совпадает ровно с набором `renderer.directx11`, `renderer.directx12` (пункты списка), `dlss*`, `network` (вкладка), `cpu_calibration`, `dlss5` (точный список составить по `launcher/main.cpp`: `grep shown\\(`); на Windows `available_on_this_platform` для всех true.
- [ ] **Step 2: Run** `ctest -R test_settings_registry` — FAIL.
- [ ] **Step 3: Implement** признак и применить в `main.cpp`; на Linux список рендерера состоит из одного пункта «Vulkan».
- [ ] **Step 4: Run** Windows PASS; Linux-тест на Deck PASS.
- [ ] **Step 5: Commit** (только по просьбе).

### Task 6: Общий `config.json` и защита значений Game Mode

**Files:**
- Modify: по результату проверки `game/uc_main.cpp` и `launcher/main.cpp`
- Test: `tests/test_options_config.cpp`

- [ ] **Step 1: Investigate** где Game Mode задаёт HDR 400/300 (поиск `peak` и `paper_white` в `game/uc_main.cpp` и `launcher/deck`): записаны ли значения в `config.json` или применяются в рантайме.
- [ ] **Step 2: Failing test** по найденному: если значения применяются в рантайме и в файл не пишутся, тест проверяет, что загрузка конфигурации десктопным видом и её сохранение не меняют поля, которые Game Mode переопределяет; иначе тест на защиту этих полей.
- [ ] **Step 3: Implement** минимальную защиту.
- [ ] **Step 4: Run** `ctest -R options_config` PASS.
- [ ] **Step 5: Commit** (только по просьбе).

### Task 7: Одна точка входа, harness и проверка на Deck

**Files:**
- Create: `launcher/main_linux.cpp`, `launcher/deck/run_deck.h`
- Modify: `launcher/deck/main_deck.cpp` (`main` → `int run_deck_launcher(int, char**)`), `launcher/CMakeLists.txt` (Linux `uc_launcher` = `main_linux.cpp` + оба вида), `launcher/deck/exec_game.cpp` и запуск в `launch_posix.cpp` (добавить `--ui <вид>`), `scripts/uctest/deck.py` (сценарии, которым нужно Deck-меню, получают `--ui deck`), `docs/steamdeck-install.md`, `docs/spikes/steamdeck-windows-validation-backlog.md`
- Test: `tests/test_deck_game.cpp` (аргументы запуска содержат `--ui deck`)

**Interfaces:**
- Consumes: `resolve_ui_mode` (T1), `run_desktop_launcher` (T4), `run_deck_launcher`.

- [ ] **Step 1: Failing test**: `build_game_command("/x", {})` содержит `--ui deck` для Deck-вида (аргумент `--ui` идёт до остальных); тест Deck-лаунчера `test_deck_game` обновлён.
- [ ] **Step 2: Implement** `main_linux.cpp`: разобрать `--ui` из `argv` (убрать из списка), вызвать `resolve_ui_mode`, при ошибке напечатать и выйти с 2; вызвать `run_desktop_launcher` или `run_deck_launcher`. Игра получает `--ui` от обоих видов.
- [ ] **Step 3: Build** Linux `uc_launcher`, `uc`; Windows `build.bat --no-install` PASS.
- [ ] **Step 4: Verify на Deck, Desktop Mode** (harness): `uc_launcher` без флага показывает Windows-лаунчер (снимок лаунчера через `--smoke`/снимок, рабочий стол не снимать, сравнить с Windows-эталоном, снятым тем же режимом); вкладок сети и пунктов DX11/DX12/DLSS нет; кнопка «Играть» запускает игру с `--ui desktop`.
- [ ] **Step 5: Verify на Deck, форсирование:** `--ui deck` в Desktop Mode показывает Deck-лаунчер и после запуска Deck-меню в игре; `--ui desktop` внутри Game Mode (ярлык Steam, `UC_DECK_GAMEMODE=1`) показывает десктопный лаунчер, окно помещается на экране 1280×800.
- [ ] **Step 6: Verify Game Mode regression:** запуск через ярлык Steam без флагов: лог `[ui] mode deck (gamescope)`, кадры Deck-вида совпадают с нынешними.
- [ ] **Step 7: Docs** `docs/steamdeck-install.md` (как запустить вне Game Mode, `--ui`/`UC_UI`), журнал.
- [ ] **Step 8: Run** `ctest --test-dir build` (Windows) и Linux-тесты на Deck PASS.
- [ ] **Step 9: Commit** (только по просьбе).

---

## Self-review

- Покрытие описания: выбор вида и переключатель (T1, T7), меню в игре (T2), лаунчер (T3–T4, T7), скрытие настроек (T5), общий config (T6), запуск как на Windows (T3), `--ui` в игру (T7), проверка Game Mode (T7 шаги 5–6), документация (T7).
- Типы: `UiMode`, `resolve_ui_mode`, `ui_mode_name` (T1) используются в T2 и T7 с теми же именами; `run_desktop_launcher` (T4) и `run_deck_launcher` (T7) совпадают с описанием.
- Риски: размер T4 (порт `main.cpp`), возможное расхождение шрифтов и масштаба на Wayland; план предусматривает итерацию по классам ошибок в T4 шаг 2.
