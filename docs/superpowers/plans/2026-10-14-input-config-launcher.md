# Input config и лаунчер: этап2

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans. Исполнение Native, уже выбрано владельцем. Шаги выполняются последовательно с RED/GREEN.

**Goal:** сохранять несколько назначений каждого действия, очищать их и редактировать через нативный лаунчер.

**Architecture:** общий config codec использует существующий `uc::Bindings`; игра и лаунчер читают один `config.json` рядом с exe. CLI имеет приоритет над сохранёнными настройками. Лаунчер передаёт игре путь config, запускает отдельный процесс и не реализует игровые действия повторно.

**Tech Stack:** C++20, Windows, существующий SDL3 3.2.0; nlohmann/json3.12.0; Dear ImGui1.92.9b, официальный SDL3 renderer backend для отдельного окна настроек. Игровой рендер остаётся Vulkan.

**Spec:** `../specs/2026-10-13-native-pc-controls-design.md`, §§2,4; исходная спека для разрешения/режимов/запуска.

Статус2026-10-14: Tasks1–3 реализованы и проверены. Полная сборка76/76CTest, actualUI smoke+controlled childlaunch exit0, liveclear/rebind, scopedLuna reviews без замечаний; dist/S установлены безconfig/saves. Дополнительный livefocus регрессионный фикс прошёл input/game_input/host_input и повторный маршрут. Task4 остаётся открытым по motion-QTE и итоговой проверке всей ветки; этап3prompt ещё впереди. Журнал `.superpowers/sdd/2026-10-14-input-config-launcher/progress.md`, публичные доказательства I2.

## Global Constraints

- Native: никакой WiiMote/IMU модели; UI редактирует существующие semantic bindings.
- Пользователь уже разрешил выполнение и сохранение всех биндов. Повторного выбора способа выполнения нет.
- Коммиты только по отдельной просьбе; никаких удалений планов/пользовательских файлов. Конкурентные texture_extract/ChaiNNer изменения сохраняются.
- Сборка в dist, установка через robocopy /E. config.json и saves пользователя не поставляются как перезаписываемые build assets.
- Диагностика видимая, со звуком и skip-intro; ввод только внутри собственного процесса. Native UI проверяется через тестовый SDL/event/model путь, без ввода ОС.
- Левые/правые модификаторы и scancode сохраняются; wheel и специальные logical combinations остаются отдельными назначениями.
- Будущие DLSS/FSR/FG не выдаются за работающие функции лаунчера до реализации соответствующих renderer stages.

## Review Focus

- Пустой список после перезапуска действительно остаётся пустым: Task1 roundtrip.
- Повреждённый/чужой config не перезаписывается при ошибке: Task1 invalid/unknown/duplicate tests и Task3 save gate.
- Несохранённые и параллельные изменения: Task1 stale document + сериализация записи; Task3 dirty/error state.
- Захват Esc, модификаторы, мышь, wheel, потеря фокуса: Task2 чистая модель + SDL events.
- Unicode path и аргументы запуска: Task1 UTF8 filesystem; Task3 quoting + CreateProcessW checks.

## Task1: общий input config и подключение игры

**Files:** Create `common/input_config.h/.cpp`, `tests/test_input_config.cpp`; Modify `common/CMakeLists.txt`, `tests/CMakeLists.txt`, `game/options.h/.cpp`, `game/uc_main.cpp`, `game/CMakeLists.txt`, `tests/test_options.cpp`.

**Interfaces:** `ConfigDocument {ordered_json properties; Bindings input; optional<string> original_bytes;}`; `parse_config(string_view)`, `load_config(path)`, `serialize_config(const ConfigDocument&)`, `save_config(path,ConfigDocument&)`. Parse/validation errors throw invalid_argument; I/O errors throw runtime_error. Maximum1MiB/depth32. Missing file/input/actions choose defaults; explicit[] clears. Stable action names equal enum identifiers. `input={version:1,bindings:{action:[...]}}`; physical `{kind:key|mouse|wheel,code:uint,modifiers:[{kind,code}]}`; specials `{kind:interact_fire|interact_motion|qte_motion}`. Unknown input members/actions/version, duplicate JSON properties, invalid codes/modifiers, >64/action fail. Other root sections roundtrip unchanged.

- [ ] RED `config_defaults_and_empty_roundtrip`: missing/default, all-actions roundtrip, multiple physical/modifier/special binds, explicit cleared list survives.
- [ ] RED `config_rejects_invalid_input`: duplicate properties at every level, signed/float code, unknown fields/action/version, depth/size, duplicate binds, illegal special action.
- [ ] RED `config_atomic_save`: Unicode path, preserved unrelated sections, reload equality, stale existing file refused, malformed original unchanged, failed write leaves original intact.
- [ ] Fetch pinned official JSON archive with SHA256 and license. Implement codec through existing validate_bindings; duplicate-key SAX/callback validation before conversion.
- [ ] Atomic save uses a writer lock, unique CREATE_NEW temporary file, FlushFileBuffers then MoveFileExW replacement; compare loaded bytes under the lock before replacing. Cleanup covers only own temporary handles/files. No automatic overwrite of unreadable/invalid user config.
- [ ] Add `--config <path>` and load default exe_dir/config.json before NativeInput setup. Bindings applied before runner; explicit CLI fields override persisted fields once Task3 adds them. Fail clearly before game starts on invalid config.
- [ ] GREEN targeted config/options/input fixtures, then visible route using isolated config. Live clear/rebind uses serialized config and same NativeInput API.

## Task2: редактирование и захват назначения

**Files:** Create `launcher/binding_editor.h/.cpp`, `tests/test_binding_editor.cpp`; Modify common action names if shared display lookup needed.

**Interfaces:** `BindingEditor` owns editable Bindings and optional Action being captured; `begin_capture(Action)`, `cancel_capture()`, `event(PhysicalInput,bool)`, `wheel(int)`, `focus(bool)`, `clear(Action)`, `restore_defaults()`. Capturing Esc cancels; explicit UI choice binds Esc. Modifier press is retained until next primary input; release-alone can bind modifier. Clear cancels pending capture for that action. All replacements validate before publishing.

- [ ] RED add/remove/multiple/clear-all including specials, defaults, duplicate rejection and no mutation on error.
- [ ] RED keyboard/mouse/wheel/modifier capture, Esc cancellation/explicitEsc, focus loss, stale capture, same binding across distinct contexts.
- [ ] Implement thin model using Bindings, no game callbacks. GREEN focused tests.

## Task3: видимый нативный лаунчер

**Files:** Create `launcher/CMakeLists.txt`, `launcher/main.cpp`, `launcher/launch.h/.cpp`, `tests/test_launch.cpp`; Modify root CMake/install rules.

Дополнительные связанные files: `game/options_config.h/.cpp`, `tests/test_options_config.cpp`; CLI defaults/configpriority/Unicodeargv в `game/options.h/.cpp`, `game/uc_main.cpp`. Интерфейс launch возвращает `GameProcess` (owned nativehandle), `game_running(const GameProcess&)` исключает двойной запуск; optional `const Options* diagnostic` разрешён только с отдельными control/savepaths и принудительным Windowed/skipintro/20sectimeout для безопасного native UI smoke.

**Interfaces:** launcher loads ConfigDocument and edits the Task2 model. Native CreateProcessW launch helper consumes exe/config paths and validated current settings; errors remain in launcher. UI saves config atomically before launch. Dear ImGui pinned source/official SDL3 platform+SDLRenderer3 backend, fonts support Russian text.

- [ ] RED Windows argv quoting for spaces, Unicode, embedded quotes/trailing slash; executable/config validation and process creation failure.
- [ ] Add tabs «Основные»/«Управление». Current settings: game folder, multiplier0.5..6 with calculated720p..8K output, Windowed/Borderless/Exclusive, skip intro, tested30/experimental60 gameplay cadence and240 presentation limit. Default resolves monitor size. Preserve unsupported future graphics sections unchanged.
- [ ] Controls show every bind, Add/remove/Clear All/restore defaults; modifier/chord/special choices, explicitEsc, visible conflicts and capture cancellation. Save error/dirty state stays visible; failed save prevents launch.
- [ ] Launch button saves then starts uc.exe with config path and settings. SDL window/render/resources released normally; no focus injection in autonomous verification.
- [ ] Verify pure editor through SDL-generated events and launched game's binding effects. Native screenshots include UI/error/capture/cleared states. Browser rules do not apply to native SDL UI.

## Task4: приёмка и установка

- [ ] Live default/cleared/rebound Grenade and Knife; multiple Fire bindings + release/focus; saved empty lists after relaunch. Process-local tests with separate config/saves.
- [ ] Complete remaining stage1 live motion-QTE calibration as encounter becomes available; record limitations explicitly.
- [ ] Full build.bat + CTest; relevant native UI/window/fullscreen/audio/render smoke. Install dist and S via /E without user config/save overwrite.
- [ ] Update I2/tech debt/checklists; fresh strongest-model final review only after plan complete. No automatic commit/push.

## Coverage / status

Spec§2→Tasks1/2/3; §4→Tasks1/3/4. Native game actions remain stage1. Port-owned prompts are mandatory stage3 with a separate plan; this stage does not change game assets.

Versions checked against official upstream release pages2026-10-14: github.com/nlohmann/json/releases/tag/v3.12.0 and github.com/ocornut/imgui/releases/tag/v1.92.9b. Existing SDL pinned3.2.0 is retained.
