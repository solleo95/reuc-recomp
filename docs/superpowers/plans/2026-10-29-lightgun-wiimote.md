# Lightgun и Wii Remote Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Абсолютный указатель для lightgun (Sinden, Gun4IR, Reaper, Dolphin Bar в режимах мыши) и родной Wii Remote (Bluetooth или Dolphin Bar режим 4) с указкой, кнопками, нунчуком и жестами.

**Architecture:** Часть A: режим `pointer=absolute` берёт позицию курсора окна вместо дельт относительной мыши и ставит её в `NativeInput::aim_absolute`. Часть B: слой HID Windows читает Wii Remote в потоке, чистый разбор отчётов и математика в `common/`, `WiimoteInput` кормит `NativeInput` через интерфейс `WiimoteTarget`, как `GamepadInput` кормит `GamepadTarget`. Демо и сеть получают готовый указатель и не меняются.

**Tech Stack:** C++20, SDL3 (окно и события), Windows HID (SetupAPI, hid.dll), ImGui (лаунчер и меню), CMake, тесты `tests/test_*.cpp`, харнесс `scripts/uctest`.

**Spec:** `docs/superpowers/specs/2026-10-29-lightgun-wiimote-design.md`

## Global Constraints

- Только Windows. Ветка `feature/lightgun-wiimote`. Коммит только по просьбе владельца.
- Исходники правятся только через Edit/Write, не через heredoc/sed (`rules/source-edits.md`). После правки со строкой с `\n` проверить строку Grep'ом до сборки.
- Относительная мышь, геймпад, демо (`Tag` формат), детерминизм и сеть не меняются; Wii Remote отключён в controlled/deterministic/demo-запусках и в режиме `--headless`.
- Тесты и прогоны в окне 1280x720 харнесса, без полноэкранного режима (`AGENTS.md`).
- Новые строки интерфейса: EN и RU через `tr()` как у соседних.
- Железа нет: всё проверяется тестами и поддельным транспортом/тестовым сервером; в итоге сказать, что живая проверка Wii Remote, Dolphin Bar и пушек не сделана.
- Конец работы: полный `build.bat` (без `--no-install`), чистка тестовых остатков в `dist`, деплой в папку игры (`rules/build-and-deploy.md`); финальное ревью на Opus (`rules/final-review-model.md`).
- Конфиг: `pointer` ("relative"|"absolute"), `pointer_smoothing` (0..90), `reload_zone` ("off"|"corner"|"edge", умолчание off), `reload_zone_size` (1..20, умолчание 5) в корне `config.json`; объект `wiimote`.

## Review Focus

- Плотность пикселей окна ≠ 1 (DPI 150%) и окно не 16:9: абсолютная точка должна совпасть с точкой относительного режима (тест в Task 1/3).
- Фокус потерян или меню открыто в абсолютном режиме: кнопки отпущены, первый клик после возврата не стреляет (Task 3).
- Отчёт Wii Remote короче нормы, неизвестного id, мусорное расширение: игнор без исключения, кнопки не залипают (Task 7).
- ИК: 0 или 1 пятно, NaN, пятна на одной точке: указатель держит последнюю позицию, не прыгает в центр (Task 8).
- Устройство занято другой программой, пропало на ходу, запись не удалась: игра работает, кнопки Wii отпущены, статус «недоступно» (Task 9).
- Геймпад и Wii Remote вместе: слоты резолвера не пересекаются (Task 10).
- Зона перезарядки: выстрел в меню, QTE и паузе не превращается в перезарядку; удержание `fire` после входа в зону и выхода не оставляет «залипший» выстрел; демо с зоной воспроизводится тем же (Task 2a).

---

### Task 1: `NativeInput::aim_absolute` и сглаживание

**Files:**
- Modify: `runtime/native_input.h`, `runtime/native_input.cpp`
- Test: `tests/test_input.cpp`

**Interfaces:**
- Produces: `bool NativeInput::aim_absolute(float px,float py,uint32_t width,uint32_t height,uint64_t timestamp_ns=0)` (px,py в пикселях окна, y вниз; false при NaN, нулевом окне или `!state_.valid`); `void NativeInput::pointer_smoothing(float fraction)` (0..0.9).

- [ ] **Step 1: Failing tests** в `tests/test_input.cpp`: `absolute_aim_maps_pixels` (окно 1000x500, `aim_absolute(500,250,..)` даёт `latest_aim()==(0,0)`; `(0,0)` даёт `(-1,-1)`; `(1000,500)` даёт `(1,1)`; за пределами зажимается); `absolute_aim_rejects_bad_input` (NaN, width 0, не в фокусе: false, указатель не меняется); `absolute_aim_feeds_gestures` (быстрый скачок на ≥600 px за 30 мс считается взмахом как `host_motion(0,-600)` в существующем тесте ножа; медленный сдвиг на 10 px нет); `smoothing_moves_part_way` (сглаживание 0.5: первая точка принимается сразу, вторая сдвигает указатель на половину).
- [ ] **Step 2: Run** `ctest --test-dir build -R test_input --output-on-failure`. Expected: FAIL (символа нет).
- [ ] **Step 3: Implement.** Указатель = clamp(2*px/width−1) по x и по y; сглаживание экспоненциальное по последней точке; дельта между двумя последними абсолютными точками (px) идёт в `actions_.motion(dx,dy,ts)` с `motion_time()` при нулевой метке; первая точка после потери фокуса дельты не даёт. `note_aim()` вызывается. Ошибки `std::overflow_error` обрабатываются как в `host_motion` (`cancel_host()`).
- [ ] **Step 4: Run** те же тесты. Expected: PASS.

### Task 2: Настройки `pointer`, `pointer_smoothing`

**Files:**
- Modify: `game/options.h`, `game/options_config.cpp`, `game/live_settings.h`, `game/live_settings.cpp`, `game/uc_main.cpp` (разбор `--pointer`, привязка цели)
- Test: `tests/test_options_config.cpp`, `tests/test_live_settings.cpp`

**Interfaces:**
- Produces: `enum class PointerMode:uint8_t { relative,absolute }` и поля `Options::pointer`, `Options::pointer_smoothing` (uint32, проценты, умолчание 0) в `game/options.h`; `LiveTargets::pointer` типа `std::function<void(PointerMode,uint32_t smoothing)>`; константа `kPointerSmoothingMax=90`. Также `enum class ReloadZone:uint8_t { off,corner,edge }`, `Options::reload_zone`, `Options::reload_zone_size` (uint32, проценты 1..20, умолчание 5), `LiveTargets::reload_zone` типа `std::function<void(ReloadZone,uint32_t size)>`.

- [ ] **Step 1: Failing tests:** `pointer_defaults_to_relative`; `pointer_round_trips` (store → load даёт то же); `pointer_rejects_unknown_value` ("pointer must be relative or absolute", `pointer_smoothing must be 0..90`); `live_pointer_change_calls_target` (изменение зовёт `targets.pointer` один раз, без изменений не зовёт); `reload_zone_defaults_off`, `reload_zone_round_trips`, `reload_zone_rejects_bad_values` ("reload_zone must be off, corner or edge", "reload_zone_size must be 1..20"), `live_reload_zone_change_calls_target`.
- [ ] **Step 2: Run** `ctest --test-dir build -R "options_config|live_settings"`. Expected: FAIL.
- [ ] **Step 3: Implement** разбор рядом с `mouse_sensitivity` в `options_config.cpp` (ошибки через `invalid(...)`), сравнение в `live_settings.cpp`, флаг `--pointer relative|absolute` рядом с остальными флагами `uc_main.cpp`.
- [ ] **Step 4: Run** те же тесты. Expected: PASS.

### Task 2a: Зона перезарядки в `NativeInput`

**Files:**
- Modify: `runtime/native_input.h`, `runtime/native_input.cpp` (`consume_actions`, рядом с обработкой `recenter`)
- Test: `tests/test_input.cpp`

**Interfaces:**
- Consumes: Task 2 (`ReloadZone` перенести в `common/input_bindings.h` или другой общий заголовок, чтобы `runtime` его видел без `game/`).
- Produces: `void NativeInput::reload_zone(ReloadZone,float size_fraction)`.

- [ ] **Step 1: Failing tests** (`tests/test_input.cpp`, контекст `gameplay`, окно 1000x500): `corner_zone_turns_fire_into_reload` (зона `corner` 5%, указатель (−0.98,−0.98), нажатие `fire` ⇒ в кадре `pressed` содержит `reload`, не содержит `fire`; на следующем кадре `reload` отпущен); `corner_zone_ignores_edge_middle` (указатель (−0.98,0) ⇒ обычный `fire`); `edge_zone_triggers_on_any_edge`; `fire_release_after_suppressed_press_is_hidden` (нажать в зоне, отпустить вне зоны ⇒ игра не видит ни `fire` held, ни `fire` released); `fire_pressed_outside_zone_then_drift_into_it_is_untouched` (нажатие вне зоны, затем указатель в зону, отпускание ⇒ обычный выстрел и отпускание); `zone_off_by_default`; `zone_inactive_in_menu_and_qte_context`; `demo_records_converted_frame` (запись и воспроизведение демо дают тот же `reload`).
- [ ] **Step 2: Run** `ctest --test-dir build -R test_input --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement** в `consume_actions` после `actions_.consume()` и до записи демо: если `context_==InputContext::gameplay` и зона включена, то по `state_.x/y` (указатель на момент чтения) проверить попадание; нажатие `fire` в зоне — убрать `fire` из `held/pressed` и из `events`, запомнить `fire_hidden_=true`, поставить `held|pressed` бита `reload` и событие `{Action::reload,true}`; следующий вызов выдаёт `released` для `reload` (флаг `reload_pulse_`); пока `fire_hidden_`, `fire` скрывается из `held`, а его отпускание убирает `fire_hidden_` и тоже скрывается. Размер зоны — доля половины окна по каждой оси (`size_fraction*2` в координатах −1..1).
- [ ] **Step 4: Run.** Expected: PASS.

### Task 3: Абсолютный путь в `HostInput`

**Files:**
- Modify: `game/host_input.h`, `game/host_input.cpp`
- Test: `tests/test_host_input.cpp`

**Interfaces:**
- Consumes: Task 1, Task 2 (`PointerMode`).
- Produces: `void HostInput::pointer(PointerMode,uint32_t smoothing_percent)`; `void HostInput::density(float pixels_per_unit)` (отношение пикселей окна к единицам событий SDL).

- [ ] **Step 1: Failing tests:** `absolute_motion_sets_aim` (режим absolute, событие `x=500,y=250` при окне 1000x500, density 1 ⇒ указатель (0,0)); `absolute_motion_respects_density` (density 2, событие x=250 ⇒ пиксель 500); `absolute_ignores_relative_deltas` (xrel без изменения позиции не двигает); `relative_mode_unchanged` (существующие тесты проходят); `absolute_first_click_after_focus_is_consumed` (захвата нет ⇒ `event()` возвращает true на первый клик, как в относительном).
- [ ] **Step 2: Run** `ctest --test-dir build -R test_host_input`. Expected: FAIL.
- [ ] **Step 3: Implement** в `HostInput::event` ветку `SDL_EVENT_MOUSE_MOTION`: в absolute вызывать `input.aim_absolute(event.motion.x*density_, event.motion.y*density_, width_, height_, event.motion.timestamp)`; `host_motion` сырых дельт в absolute не вызывать (жесты уже питаются из Task 1).
- [ ] **Step 4: Run.** Expected: PASS.

### Task 4: Захват курсора окном и UI

**Files:**
- Modify: `game/uc_main.cpp` (apply_host_keys, цикл событий, recapture), `launcher/main.cpp` (рядом со слайдером чувствительности, `main.cpp:852`), `game/settings_menu.cpp` (рядом со слайдером, `settings_menu.cpp:427`), строки `tr` лаунчера и меню (EN и RU), `launcher/main.cpp:169` (сравнение настроек)
- Test: `tests/test_settings_menu.cpp`

**Interfaces:**
- Consumes: Tasks 2, 3.
- Produces: `capture_pointer(bool playing)` в `uc_main.cpp`: в relative `SDL_SetWindowRelativeMouseMode(playing)`; в absolute `SDL_SetWindowRelativeMouseMode(false)`, `SDL_SetWindowMouseGrab(playing)`, `SDL_HideCursor`/`SDL_ShowCursor` по `playing`.

- [ ] **Step 1: Failing test** в `test_settings_menu.cpp`: `pointer_combo_edits_options` (выбор «Lightgun» ставит `options.pointer==PointerMode::absolute`, слайдер сглаживания виден только в absolute и держит 0..90); `reload_zone_controls_edit_options` (комбо «Выкл / Угол / Край» ставит `options.reload_zone`; слайдер размера зоны 1..20% виден, только когда зона не `off`). Подписи: «Перезарядка выстрелом у края» / «Reload by shooting at the edge», «Угол» / «Corner», «Край» / «Edge», «Размер зоны» / «Zone size». `live_settings` передаёт значения в `NativeInput::reload_zone`.
- [ ] **Step 2: Run** `ctest --test-dir build -R test_settings_menu`. Expected: FAIL.
- [ ] **Step 3: Implement.** Подписи: «Указывающее устройство» / «Pointing device»: «Мышь» / «Mouse», «Lightgun (абсолютный указатель)» / «Lightgun (absolute pointer)»; «Сглаживание прицела» / «Aim smoothing». Все прежние вызовы `SDL_SetWindowRelativeMouseMode` в `uc_main.cpp` (стр. ~515, 909, 1011, 1077) заменить на `capture_pointer`. Передать `HostInput::pointer` и `density` (`SDL_GetWindowPixelDensity`) при старте и из `live_settings`. Состояние «captured» для `host_input.event` в absolute = окно в фокусе и игра идёт (не меню, не пауза).
- [ ] **Step 4: Run** тесты меню. Expected: PASS. Затем `build.bat --no-install`, убедиться, что собирается.

### Task 5: Тестовый протокол и сценарий `lightgun`

**Files:**
- Modify: `game/test_protocol.h`, `game/test_protocol.cpp`, `game/test_server.cpp`, `runtime/native_input.h` (`HostEventKind::aim`), `runtime/native_input.cpp` (`apply_test_event`), `scripts/uctest/client.py`, `scripts/uctest/scenarios.py`
- Test: `tests/test_test_protocol.cpp`, `tests/test_test_server.cpp`, `tests/test_native_pad.cpp`

**Interfaces:**
- Produces: устройство протокола `"aim"` с телом `{"x":-1..1,"y":-1..1}` (абсолют, как у `latest_aim()`), `HostEventKind::aim`, сценарий `python -m uctest run lightgun` (ставит указатель в 5 точек и проверяет `aim` в кадре теста с допуском 0.01; жест взмаха через два быстрых `aim`; выстрел в углу при `reload_zone=corner` (опция через `pumped.options`) вызывает перезарядку: патроны в обойме возвращаются, выстрел в центре не перезаряжает).

- [ ] **Step 1: Failing tests:** `test_protocol` разбирает `{"aim":{"x":0.5,"y":-0.25}}` и отвергает `x:2`; `apply_test_event` с `aim` ставит указатель.
- [ ] **Step 2: Run** `ctest -R "test_protocol|test_server|native_pad"`. Expected: FAIL.
- [ ] **Step 3: Implement** по образцу `pad_stick` (`test_protocol.cpp:145`), правка протокола в `docs/testing/test-server.md`.
- [ ] **Step 4: Run** тесты PASS; затем `cd scripts && python -m uctest run lightgun --speed 8` (один прогон). Expected: сценарий проходит.

### Task 6: Документация части A

**Files:**
- Modify: `README.md`, `README.ru.md`
- Create: `docs/lightguns.md`

- [ ] **Step 1:** Раздел «Lightgun и абсолютный указатель»: когда включать «Lightgun», что делать с Sinden (безрамочный режим, рамка программы Sinden), Reaper (режим мыши, калибровка своей программой), Gun4IR (режим мыши), Dolphin Bar режимы 1/2; таблица режимов Dolphin Bar (1, 2: мышь; 3: геймпад без указки; 4: родной Wii Remote, Task 7–13); привязка перезарядки и ножа к кнопкам пушки; перезарядка выстрелом в угол или у края (как включить, зачем выключена по умолчанию: враг в углу не получит выстрел; Sinden и Gun4IR могут слать свой «выстрел за экран» правой кнопкой, тогда достаточно привязки `reload` на правую кнопку мыши); чего не умеет (два ствола).
- [ ] **Step 2:** Проверить ссылки и таблицы глазами.

### Task 7: Протокол Wii Remote (чистый разбор)

**Files:**
- Create: `common/wiimote_protocol.h`, `common/wiimote_protocol.cpp`
- Modify: `common/CMakeLists.txt` (цель `uc_wiimote_protocol`), `tests/CMakeLists.txt`
- Test: `tests/test_wiimote_protocol.cpp`

**Interfaces:**
- Produces:
  - `enum class WiiButton:uint8_t { left,right,down,up,plus,two,one,b,a,minus,home,nunchuk_z,nunchuk_c,count }`
  - `struct WiiIrDot { uint16_t x,y; bool valid; }`; `struct WiiState { uint32_t buttons; std::array<uint8_t,3> accel; std::array<WiiIrDot,4> ir; bool has_nunchuk; std::array<uint8_t,2> stick; std::array<uint8_t,3> nunchuk_accel; }`
  - `bool wii_parse_report(std::span<const uint8_t> report,WiiState& state)` — для id 0x30, 0x31, 0x33, 0x35, 0x37; false для короткого, неизвестного или пустого; не бросает.
  - Строители: `std::array<uint8_t,2> wii_led_report(unsigned player)`; `wii_reporting_report(uint8_t mode)` (0x12, непрерывный); `std::vector<uint8_t> wii_write_register(uint32_t address,std::span<const uint8_t> data)` (0x16, бит 0x04 в первом байте адреса, до 16 байт, иначе `std::invalid_argument`); `wii_read_register`; `std::vector<WiiStep> wii_init_sequence()` — упорядоченные записи инициализации: расширение (0x55 → 0xA400F0, 0x00 → 0xA400FB), ИК (0x04 → 0x13 и 0x1A, 0x08 → 0xB00030, блок чувствительности 1 в 0xB00000, блок 2 в 0xB0001A, режим 1 → 0xB00033, 0x08 → 0xB00030), режим отчёта 0x37, светодиод; у шага `delay_ms` (≥50 после записей ИК).
  - `uint16_t` кнопки: маска первого байта 0x01 влево, 0x02 вправо, 0x04 вниз, 0x08 вверх, 0x10 плюс; второго 0x01 два, 0x02 один, 0x04 B, 0x08 A, 0x10 минус, 0x80 Home.

- [ ] **Step 1: Failing tests** (по записанным байтам): `parses_buttons` (0x30 с A+B+Home даёт ровно эти биты); `parses_0x37_with_ir_and_nunchuk` (4 пятна по 10 бит, стик и Z/C инвертированы: в нунчуке биты кнопок равны 0 при нажатии); `ir_dot_ffs_is_invalid` (0xFF,0xFF,0xFF ⇒ `valid=false`); `rejects_short_and_unknown` (длины 0, 1, обрезанный 0x37; id 0x3E); `write_register_layout` (0x16, 0x04|адрес-старший, 16-байтное заполнение, длина 22); `init_sequence_order` (расширение до ИК, режим отчёта последним перед LED, паузы ≥50 мс после записей ИК); `led_report_player_one`.
- [ ] **Step 2: Run** `ctest -R wiimote_protocol`. Expected: FAIL.
- [ ] **Step 3: Implement** по таблицам Wiibrew (раскладки в дизайне: `docs/superpowers/specs/...` раздел «Что показало исследование», точные биты выше).
- [ ] **Step 4: Run.** Expected: PASS.

### Task 8: Математика указки, нунчука и жестов

**Files:**
- Create: `common/wiimote_math.h`, `common/wiimote_math.cpp`
- Modify: `common/CMakeLists.txt`
- Test: `tests/test_wiimote_math.cpp`

**Interfaces:**
- Consumes: `WiiIrDot` (Task 7).
- Produces: `std::optional<std::array<float,2>> wii_ir_pointer(const std::array<WiiIrDot,4>&)` — середина двух ближайших валидных пятен, x зеркалится, нормировка 1024x768 → −1..1 (y вниз), компенсация крена по углу между пятнами; nullopt при <2 пятен или совпадающих точках; `std::array<float,2> wii_stick(std::array<uint8_t,2> raw,float dead_zone)` (центр 128 с калибровкой по умолчанию 0x80±0x60, −1..1, y вверх); `class WiiSwing { public: std::array<float,2> feed(std::array<uint8_t,3> accel,uint64_t ns,float threshold); }` — возвращает «счёты движения» (пиксели) для детекторов ножа/встряски: величина (|a|−1g) выше порога даёт импульс по оси сильнейшего ускорения.

- [ ] **Step 1: Failing tests:** `pointer_centered_dots_gives_zero`; `pointer_mirrors_x` (пятна левее ⇒ указатель правее); `pointer_none_or_one_dot_is_nullopt`; `pointer_roll_compensated` (пятна под углом 30°, центр тот же); `stick_dead_zone_and_clamp`; `swing_ignores_gravity_and_small_motion` (покой 0x80,0x80,0xB3 ⇒ 0); `swing_big_jerk_emits_counts` (скачок по x ⇒ ненулевые счёты, знак по оси).
- [ ] **Step 2: Run** `ctest -R wiimote_math`. Expected: FAIL.
- [ ] **Step 3: Implement.** Масштаб счётов подобрать так, чтобы взмах ≥ `kKnifeSpeed` (см. `runtime/` детекторов) пробивал порог по умолчанию, а наведение нет; значение записать константой с комментарием.
- [ ] **Step 4: Run.** Expected: PASS.

### Task 9: Транспорт HID, рукопожатие, `WiimoteInput`

**Files:**
- Create: `common/wiimote_target.h`, `game/wiimote_transport.h`, `game/wiimote_hid.cpp`, `game/wiimote_input.h`, `game/wiimote_input.cpp`
- Modify: `game/CMakeLists.txt`
- Test: `tests/test_wiimote_input.cpp`

**Interfaces:**
- Produces:
  - `class WiimoteTarget { virtual void host_button(PhysicalInput,bool)=0; virtual void host_wii_pointer(float x,float y,uint64_t ns)=0; virtual void host_wii_stick(float x,float y)=0; virtual void host_wii_motion(float dx,float dy,uint64_t ns)=0; virtual void wii_reset()=0; }` (NativeInput реализует в Task 10).
  - `struct WiiTransport { virtual bool read(std::vector<uint8_t>& report,unsigned timeout_ms)=0; virtual bool write(std::span<const uint8_t>)=0; virtual ~WiiTransport()=default; }` — `wiimote_hid.cpp`: перечисление HID 057E:0306 и 057E:0330 (SetupDi + `HidD_GetAttributes`), `CreateFile` с `FILE_FLAG_OVERLAPPED`, `ReadFile`, запись `WriteFile`, при ошибке `HidD_SetOutputReport` (запоминает рабочий способ), `std::unique_ptr<WiiTransport> wii_open_first()`.
  - `class WiimoteInput { WiimoteInput(std::function<std::unique_ptr<WiiTransport>()> opener); ~WiimoteInput(); void update(WiimoteTarget&,bool active,const WiimoteSettings&); WiimoteStatus status() const; }` — поток: открывает устройство раз в 2 с пока нет, шлёт `wii_init_sequence()`, читает отчёты через `wii_parse_report`; `update` в основном потоке переводит последнее состояние в вызовы цели (изменения кнопок как `PhysicalInput{PhysicalKind::wii_button,code}`, указка из `wii_ir_pointer`, стик, взмах); `active=false` ⇒ `wii_reset()`. `struct WiimoteStatus { bool connected; bool has_nunchuk; std::string error; }`.

- [ ] **Step 1: Failing tests** с поддельным транспортом: `handshake_writes_init_sequence_in_order`; `report_flows_to_target` (0x37 с A ⇒ `host_button(wii_button A,true)`, затем отпускание); `lost_ir_keeps_pointer`; `disconnect_releases_everything` (read возвращает false ⇒ `wii_reset`, статус `connected=false`); `write_failure_reports_error_and_game_continues`; `inactive_resets`; `reopens_after_loss` (opener вызывается снова).
- [ ] **Step 2: Run** `ctest -R wiimote_input`. Expected: FAIL.
- [ ] **Step 3: Implement.** Поток с `std::atomic` стопом; общий снимок под мьютексом; нет исключений через границу потока (`catch` → `status.error`). `wiimote_hid.cpp` не покрыт тестами (железо) — вручную проверить на сборке, что перечисление ничего не находит и не падает без устройства.
- [ ] **Step 4: Run.** Expected: PASS.

### Task 10: `NativeInput`, привязки и конфиг Wii Remote

**Files:**
- Modify: `common/input_bindings.h/.cpp` (`PhysicalKind::wii_button`, слоты 832..844 резолвера, `default_wii_bindings()`, `validate_wii_bindings`, `merge_bindings` на три таблицы), `common/input_config.cpp` (чтение `wiimote.bindings`), `common/gamepad_config.*` или новый `common/wiimote_config.h/.cpp` (`struct WiimoteSettings { bool enabled=true; float smoothing=.35f; float swing_threshold; float stick_dead_zone=.10f; Bindings bindings=default_wii_bindings(); }`, `wiimote_from_config`, `store_wiimote`), `runtime/native_input.h/.cpp` (реализует `WiimoteTarget`, `wiimote_settings(const WiimoteSettings&)`), `game/options.h`, `game/options_config.cpp`, `game/live_settings.*`
- Test: `tests/test_input_bindings.cpp`, `tests/test_wiimote_config.cpp`, `tests/test_native_pad.cpp`

**Interfaces:**
- Consumes: Tasks 1, 7–9.
- Produces: умолчания по оригиналу: B (триггер) `fire`; A `interact`; 1 `reload`; 2 `grenade`; плюс `weapon_next`; минус `weapon_previous`; Home `pause`; крестовина вверх/вниз `weapon_next`/`weapon_previous`; нунчук Z `knife`; C `reload`; A также `confirm`, B `back`. `host_wii_stick` питает аналоговую камеру (`update_camera`), `host_wii_pointer` идёт через `aim_absolute`-путь с `smoothing`, `host_wii_motion` в `actions_.motion`.

- [ ] **Step 1: Failing tests:** `wii_defaults_validate`; `merge_three_tables_keeps_each` (клавиатура, геймпад, Wii на одно действие все живы); `wii_slots_do_not_collide_with_pad` (Review Focus); `wii_button_drives_action` (хост-кнопка B ⇒ действие `fire` в кадре); `wii_stick_sets_camera` (стик вправо ⇒ camera x>0); `wii_pointer_moves_aim`; `wiimote_config_round_trip`; `wiimote_config_rejects_unknown_key`.
- [ ] **Step 2: Run** `ctest -R "input_bindings|wiimote_config|native_pad"`. Expected: FAIL.
- [ ] **Step 3: Implement** по образцу геймпада (`pad_button` → `wii_button`). Демо не затрагивается: стик Wii идёт через существующий `camera_`, указатель готовым состоянием.
- [ ] **Step 4: Run.** Expected: PASS; также прежние `ctest` по входам.

### Task 11: Подключение в игру и тестовый сервер

**Files:**
- Modify: `game/uc_main.cpp` (создать `WiimoteInput` кроме controlled/headless/demo/deterministic, `update` каждый цикл рядом с `gamepad->update`, `refresh_prompt_device`), `game/test_protocol.*`, `game/test_server.cpp`, `runtime/native_input.cpp` (`apply_test_event`), `scripts/uctest/client.py`, `scripts/uctest/scenarios.py`, `docs/testing/test-server.md`
- Test: `tests/test_test_protocol.cpp`, `tests/test_native_pad.cpp`

**Interfaces:**
- Produces: устройства протокола `wii_button` (`{"button":0..12,"down":bool}`), `wii_pointer` (`{"x","y"}` −1..1), `wii_stick`, `wii_motion` (`{"x","y"}` счёты); сценарий `wiimote` (кнопка B стреляет, указка двигает прицел, стик двигает камеру).

- [ ] **Step 1: Failing tests:** разбор и применение четырёх устройств; отказ вне диапазона.
- [ ] **Step 2: Run** `ctest -R "test_protocol|native_pad"`. Expected: FAIL.
- [ ] **Step 3: Implement**; один прогон `python -m uctest run wiimote --speed 8`.
- [ ] **Step 4: Run** тесты и сценарий. Expected: PASS.

### Task 12: Окно «Wii Remote», подсказки, глифы

**Files:**
- Create: `launcher/wiimote_panel.h/.cpp`, `launcher/wii_glyphs.h/.cpp` (процедурные глифы A, B, 1, 2, +, −, Home, крестовина, Z, C в `ImDrawList`, как `pad_glyphs`)
- Modify: `launcher/main.cpp`, `game/settings_menu.cpp` (кнопка «Wii Remote» на вкладке «Контроллеры» рядом с «Gamepad», `launcher/gamepad_panel.cpp:256`), `common/input_prompts.cpp` и `game/prompt_hooks`/`prompt_bridge` (подсказки Wii, когда последнее устройство Wii Remote), строки EN/RU
- Test: `tests/test_binding_editor.cpp`, `tests/test_input_prompts.cpp`, `tests/test_pad_glyphs.cpp` (или `tests/test_wii_glyphs.cpp`)

**Interfaces:**
- Consumes: Task 10 (таблица `wiimote.bindings`, `WiimoteSettings`), `WiimoteStatus` (Task 9).
- Produces: `bool wiimote_modal(WiimotePanelState&,WiimoteSettings&,BindingEditor&,const WiimoteStatus&,float unit,const PanelText&)` по образцу `modal` в `gamepad_panel.cpp`: статус («Wii Remote не найден», «Подключён, нунчук есть»), переключатель включения, сглаживание, порог взмаха, мёртвая зона стика, строки привязок, «По умолчанию». Захват кнопки Wii идёт через приёмник `WiimoteInput`.

- [ ] **Step 1: Failing tests:** `wii_glyph_label_for_each_button` (подписи «A», «B», «1», «2», «+», «−», «Home», «Z», «C», стрелки); `prompt_uses_wii_table_when_last_device_wii` (описание действия `fire` для Wii даёт «B»); `editor_captures_wii_button` (второй `BindingEditor` принимает `wii_button` и отвергает `key`).
- [ ] **Step 2: Run** `ctest -R "binding_editor|input_prompts|glyphs"`. Expected: FAIL.
- [ ] **Step 3: Implement** по образцу геймпадной панели; общий код ползунков и комбо не копировать, вынести в общий заголовок только если gamepad_panel уже экспортирует их, иначе скопировать минимально.
- [ ] **Step 4: Run.** Expected: PASS. Вручную: открыть лаунчер и внутриигровое меню в окне 1280x720 харнессом, снять картинку окна «Wii Remote» через `uctest` (скриншот) и посмотреть.

### Task 13: Документация части B

**Files:**
- Modify: `README.md`, `README.ru.md`, `docs/lightguns.md` (раздел Wii Remote), `docs/modding/api/input.md` если влияет на API (проверить grep на `pad_`)

- [ ] **Step 1:** Подключение: Bluetooth Windows (1+2 или красная SYNC, без PIN), Dolphin Bar режим 4, что делать, если Steam Input или Dolphin уже заняли устройство; раскладка по умолчанию; указка и датчик-бар; нунчук; жесты; чего нет (Motion Plus, динамик, вибрация, два пульта); честная пометка «не проверено на железе, нужна обратная связь».
- [ ] **Step 2:** Проверить таблицы и ссылки.

### Task 14: Сборка, проверка, деплой, ревью

- [ ] **Step 1:** `& "D:\Projects\Native Ports\Umbrella-chronicles-recomp\build.bat"` из PowerShell (полный, без `--no-install`); все тесты зелёные (`UC_ALL_TESTS=1` один раз, чтобы увидеть полный набор).
- [ ] **Step 2:** Один прогон `uctest` сценариев `lightgun`, `wiimote`, `controls`, `gamepad` на 1280x720; прежний сценарий `train_gameplay` — регресс относительной мыши.
- [ ] **Step 3:** Убрать остатки тестов из `dist` (дампы, примеры модов), проверить, что деплой в папку игры прошёл; записать путь и время.
- [ ] **Step 4:** Финальное ревью свежим субагентом с `model: "opus"` (Review Focus выше как задание); исправления; повторный `build.bat`.
- [ ] **Step 5:** Памятка владельцу: что не проверено (железо), как собрать обратную связь от игроков; предложить записать заметку в память проекта (`wiimote` и lightgun итоги).
