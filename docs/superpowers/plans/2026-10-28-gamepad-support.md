# Gamepad Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Любой стандартный геймпад (Xbox, DualSense, Switch Pro, сторонние) управляет игрой: прицел стиком и гироскопом, аналоговая камера правым стиком, жесты, переназначение, вкладка «Контроллеры» с модальным окном «Gamepad», подсказки кнопок в стиле родного геймпада.

**Architecture:** SDL3 Gamepad API (`game/gamepad_input.*`) подаёт сырые события в `NativeInput`; вся логика (роли стиков, кривые, гироскоп, центрирование) живёт в `NativeInput` и чистой математике `common/gamepad_math.*`, поэтому тестовый сервер и SDL идут одним путём. Привязки геймпада это вторая таблица `Bindings` с входами `pad_button`/`pad_axis`, `BindingResolver` получает объединение таблиц. Камера становится аналоговой через `ActionFrame.camera` (int8 x2) с версией формата демо и сетевого кадра.

**Tech Stack:** C++20, SDL3 (gamepad, sensors, virtual joystick для тестов), ImGui, nlohmann::json, CMake/ctest, harness `uctest`.

**Spec:** `docs/superpowers/specs/2026-10-28-gamepad-support-design.md`

## Global Constraints

- Вкладка «Управление» остаётся только для клавиатуры и мыши; всё про контроллеры только во вкладке «Контроллеры» (лаунчер и игра).
- Привязки геймпада хранятся в `config.json` объект `gamepad` (`gamepad.bindings`), ключ `input.bindings` не меняется; старый `config.json` без `gamepad` и без ключа `recenter` читается и получает умолчания.
- Привязка геймпада содержит только входы геймпада; клавиатурная таблица не содержит входов геймпада; проверка конфликтов внутри каждой таблицы.
- Кнопки хранятся по позиции SDL (south/east/west/north); подписи следуют семейству геймпада.
- Умолчания (таблица геймпада): `fire` RT, `interact` south, `reload` west, `knife` east, `grenade` north, `weapon_previous` LB, `weapon_next` RB, `pause` Start, `confirm` south, `back` east, `screenshot` Back/View, `recenter` L3, `qte_shake` R3; `release_cursor` и D-pad без назначения.
- Стик прицела по умолчанию левый, стик камеры правый, роли не могут совпадать; мёртвая зона прицела 15%, степень кривой 2, мёртвая зона камеры 10%.
- Камера: int8 -127..127; старые демо воспроизводятся как раньше (ось из битов `camera_*`, когда оба значения 0); протокол сети и маркер кадра поднимаются до версии 2.
- Гироскоп по умолчанию «Выкл»; режимы «Выкл», «Всегда», «Пока зажата кнопка» (умолчание левый курок); жесты по гироскопу работают при режиме не «Выкл».
- Все подключённые геймпады управляют одним игроком; отключение отпускает все кнопки геймпада; пока открыто меню игры события геймпада не двигают игровой указатель.
- Подсказки: тип управления «Авто» / «Клавиатура и мышь» / «Геймпад», модель «Авто» / Xbox / PlayStation / Nintendo / «Универсальный»; глифы рисуются в оригинальном стиле семейства и не зависят от `PromptStyle`.
- Правила проекта: правки исходников только через Edit/Write (без heredoc, python, sed); тесты и сценарии только в окне 1280x720 (никакого фуллскрина), живых прогонов один-два; сборка `build.bat --no-install`, тесты `ctest --test-dir build --output-on-failure`; коммиты и пуши только по просьбе владельца (шагов commit в плане нет).

## Review Focus

1. Геймпад отключили, пока зажаты стрельба/курок или отклонён стик: все кнопки отпущены, указатель не дрейфует (Task 5, Task 6).
2. Два геймпада подключены, у одного дрейф стика: суммирование не даёт дрейфу двигать прицел выше мёртвой зоны (Task 5).
3. Старый `config.json` без `gamepad` / `recenter`; неизвестный ключ или значение вне диапазона в `gamepad` отклоняется с ошибкой, а не падением; смена версии демо/сети не ломает старые демо (Task 3, Task 4).
4. Геймпад лежит на столе: шум гироскопа ниже мёртвой зоны не двигает прицел, калибровка смещения после подключения (Task 2, Task 5).
5. Открыто меню или потеря фокуса при отклонённом стике: указатель стоит, после закрытия нет скачка (ограничение dt) (Task 5, Task 6).

---

### Task 1: Входы геймпада в привязках

**Files:**
- Modify: `common/input_bindings.h`, `common/input_bindings.cpp`, `common/input_config.h` (`action_names`), `common/input_config.cpp`
- Test: `tests/test_input_bindings.cpp`, `tests/test_input_config.cpp`

**Interfaces:**
- Produces (`common/input_bindings.h`):
  - `enum class PhysicalKind:uint8_t { key,mouse,wheel,pad_button,pad_axis };` код `pad_button` = значение `SDL_GamepadButton`; код `pad_axis` = `SDL_GamepadAxis*2 + (положительное направление ? 1 : 0)`.
  - `enum class PadFamily:uint8_t { xbox,playstation,nintendo,generic };` (здесь, чтобы общий заголовок видели Task 4, 6, 7)
  - `Action::recenter` добавляется перед `count` (последним).
  - `Bindings default_pad_bindings();` (умолчания из Global Constraints; повторить вид привязок `fire`/`interact` из `default_bindings()`, если там `interact_fire`)
  - `std::optional<std::string> validate_pad_bindings(const Bindings&);` (клавиатурные/мышиные входы отклоняются)
  - `Bindings merge_bindings(const Bindings& keyboard,const Bindings& pad);` (для каждого действия сначала клавиатурные, затем геймпадные)
  - `std::array<int8_t,2> camera{}` поле в `ActionFrame` объявляется в Task 3, не здесь.
  - `validate_bindings` отклоняет входы `pad_*`.
- `BindingResolver::button(PhysicalInput,bool)` принимает новые виды (битсеты `pad_buttons_` 32, `pad_axes_` 16).

- [ ] **Step 1: Тесты**: `pad_defaults_validate` (`validate_pad_bindings(default_pad_bindings())` пусто; `default_pad_bindings()[Action::fire]` содержит `{pad_axis,11}`); `keyboard_table_rejects_pad_input`; `pad_table_rejects_key`; `merged_resolver_fires_from_pad_and_mouse` (resolver с `merge_bindings(default_bindings(),default_pad_bindings())`, контекст gameplay, `button({pad_axis,11},true)` → `consume().held` содержит бит `Action::fire`); `pad_shoulder_buttons_map_weapon_actions`; в `test_input_config`: старый документ без ключа `recenter` парсится, действие пустое; `serialize_config` пишет `recenter`.
- [ ] **Step 2:** `cmake --build build --target test_input_bindings test_input_config`, `ctest --test-dir build -R "test_input_bindings|test_input_config"`. Ожидание: FAIL (нет символов).
- [ ] **Step 3: Реализация** по сигнатурам выше; `allowed(Action::recenter)` только в `gameplay`; `action_names` получает `recenter`; парсер config не требует ключ `recenter` (отсутствие равно пустому списку).
- [ ] **Step 4:** те же тесты. Ожидание: PASS.

### Task 2: Чистая математика геймпада

**Files:**
- Create: `common/gamepad_math.h`, `common/gamepad_math.cpp`, `tests/test_gamepad_math.cpp`
- Modify: `common/CMakeLists.txt` (цель `uc_gamepad_math`), `tests/CMakeLists.txt` (`add_test`)

**Interfaces:**
- Produces:
```cpp
namespace uc {
enum class GyroMode:uint8_t { off,always,hold };
struct StickCurve { float dead_zone=.15f, speed=1.2f; };   // speed: доля высоты экрана в секунду при полном отклонении
struct GyroSettings { GyroMode mode=GyroMode::off; float sensitivity_x=1,sensitivity_y=1,dead_zone=.0087f; bool invert_x=false,invert_y=false; };  // dead_zone рад/с (0,5 градуса)
constexpr float kGyroCountsPerRadian=2000.f;   // условные отсчёты мыши на радиан; подобрать так, чтобы взмах ножом срабатывал по kKnifeSpeed, а наводка нет
std::array<float,2> stick_aim_velocity(float x,float y,const StickCurve&);   // доли высоты экрана в секунду; радиальная мёртвая зона, ресэмпл, степень 2
std::array<int8_t,2> camera_axes(float x,float y,float dead_zone);           // радиальная мёртвая зона, ресэмпл, округление к ±127
class GyroBias { public: std::array<float,2> filter(float yaw,float pitch,uint64_t ts_ns); };  // первые 0,5 с покоя усредняются в смещение; дальше медленная подстройка при |rate|<2*dead_zone
std::array<float,2> gyro_aim_counts(float yaw,float pitch,float dt,const GyroSettings&);  // (отсчёты x, отсчёты y) с инверсией и чувствительностью; ниже dead_zone нули
}
```

- [ ] **Step 1: Тесты** (`stick_inside_dead_zone_is_zero`: `stick_aim_velocity(.1f,0,{})` равно `{0,0}`; `stick_full_deflection_is_speed`: `(1,0)` даёт `{1.2f,0}` с допуском 1e-4; `stick_curve_is_quadratic_after_rescale`: отклонение .575 при зоне .15 → 0.25*speed*... ровно `((.575-.15)/.85)^2*1.2`; `camera_axes(1,0,.1f)=={127,0}`; `camera_axes(.05f,.05f,.1f)=={0,0}`; `camera_axes(-1,1,.1f)=={-127,127}` для диагонали клэмпится по радиусу (вектор длины 1.41 нормируется до 1: проверить `{-90,90}` с допуском 1); `gyro_below_dead_zone_is_zero`; `gyro_sensitivity_and_invert`; `gyro_bias_cancels_constant_offset` (поток 0,5 с постоянного смещения 0,02 рад/с, затем `filter` того же значения даёт |x|<1e-3)).
- [ ] **Step 2:** FAIL (нет цели).
- [ ] **Step 3: Реализация** `gamepad_math.cpp` по сигнатурам; без SDL и без глобального состояния.
- [ ] **Step 4:** `ctest -R test_gamepad_math` PASS.

### Task 3: Аналоговая камера, версии демо и сети

**Files:**
- Modify: `common/input_bindings.h/.cpp` (`ActionFrame::camera`, клавиатура даёт ±127 в `consume()`), `runtime/native_input.h/.cpp` (демо), `game/game_input.cpp` (`game_input_buttons`), `game/netplay_game.cpp/.h`, `common/netplay.cpp:76`
- Test: `tests/test_input.cpp`, `tests/test_game_input.cpp`, тест netplay-кадра (найти существующий для `encode_net_frame`, `tests/CMakeLists.txt`)

**Interfaces:**
- Produces: `ActionFrame::camera` (`std::array<int8_t,2>`, x вправо плюс, y вверх плюс, как `stick_x/stick_y`); `NativeInput` демо-тег `Tag::camera=6` (запись только когда камера не нулевая, сразу после записи `actions`); `encode_net_frame`/`decode_net_frame` версия 2 (маркер `'U','C','F',2`, два байта камеры после событий; версия 1 отклоняется); `protocol_version=2`.
- Consumes: `bit(Action::camera_*)` как раньше.
- `game_input_buttons`: если `frame.camera` ненулевая, `stick_x=camera[0]/127.f`, `stick_y=camera[1]/127.f`; иначе прежний расчёт из битов.

- [ ] **Step 1: Тесты:** `camera_axes_default_from_keys` (клавиши camera_right → `consume_actions().camera=={127,0}`); `game_input_uses_analog_camera` (`camera={64,-127}` даёт `stick_x≈0.504f`, `stick_y==-1.f`); `game_input_falls_back_to_bits` (camera нулевая, бит camera_left → `-1`); `old_demo_without_camera_tag_replays` (файл демо, записанный фикстурой из старого формата, т.е. без тега 6, воспроизводится, камера нулевая); `new_demo_roundtrips_camera`; `net_frame_v2_roundtrips_camera`; `net_frame_v1_is_rejected`.
- [ ] **Step 2:** запуск тестов, FAIL.
- [ ] **Step 3: Реализация** по интерфейсам; размер `InputSample` и `kDemoSampleBytes` не менять.
- [ ] **Step 4:** PASS; выполнить также `ctest --test-dir build -R "demo|netplay|input"` и убедиться, что прежние тесты зелёные.

### Task 4: Настройки геймпада и config.json

**Files:**
- Create: `common/gamepad_config.h`, `common/gamepad_config.cpp`, `tests/test_gamepad_config.cpp`
- Modify: `common/input_config.h/.cpp` (вынести `nlohmann::ordered_json bindings_to_json(const Bindings&)` и `Bindings bindings_from_json(const nlohmann::json&)` из `serialize_config`/`parse_config`, поведение без изменений), `game/options.h` (`GamepadSettings gamepad;`), `game/options_config.cpp` (`options_from_config`, `store_options`), `game/live_settings.h/.cpp` (`SettingsTargets::gamepad`, диф и `apply`), `common/CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/test_options_config.cpp`, `tests/test_live_settings.cpp`

**Interfaces:**
- Produces (`common/gamepad_config.h`):
```cpp
enum class ControlType:uint8_t { automatic,keyboard_mouse,gamepad };
enum class PadModel:uint8_t { automatic,xbox,playstation,nintendo,generic };
enum class StickRole:uint8_t { none,left,right };
struct GamepadSettings {
    ControlType type=ControlType::automatic; PadModel model=PadModel::automatic;
    StickRole aim_stick=StickRole::left, camera_stick=StickRole::right;
    StickCurve aim; float camera_dead_zone=.10f; GyroSettings gyro;
    PhysicalInput gyro_hold{PhysicalKind::pad_axis,9};
    Bindings bindings=default_pad_bindings();
    bool operator==(const GamepadSettings&) const=default;
};
std::optional<std::string> validate_gamepad(const GamepadSettings&);   // диапазоны, роли не совпадают, validate_pad_bindings
GamepadSettings gamepad_from_config(const ConfigDocument&);            // нет объекта gamepad = умолчания; ошибка = исключение так же, как у соседних разборов options_config
void store_gamepad(ConfigDocument&,const GamepadSettings&);
```
- Формат: `config.json` → `"gamepad": {"type","model","aim_stick","camera_stick","dead_zone","aim_speed","camera_dead_zone","gyro":{"mode","sensitivity_x","sensitivity_y","dead_zone","invert_x","invert_y","hold"},"bindings":{action:[...]}}`; неизвестные ключи отклоняются; диапазоны: dead_zone 0..0.5, aim_speed 0.2..4, sensitivity 0.1..10, gyro dead_zone 0..0.05.

- [ ] **Step 1: Тесты:** `missing_gamepad_object_gives_defaults`; `roundtrip_preserves_every_field`; `unknown_key_is_rejected`; `same_stick_role_is_rejected`; `out_of_range_values_are_rejected` (по полю); `keyboard_input_in_pad_bindings_is_rejected`; `input_section_unchanged_by_gamepad_store` (запись `gamepad` не меняет байты `input`); в `test_live_settings`: `apply(Options)` с изменённым `gamepad` вызывает ровно цель `gamepad` и сохраняет.
- [ ] **Step 2:** FAIL.
- [ ] **Step 3: Реализация** по интерфейсам.
- [ ] **Step 4:** `ctest -R "gamepad_config|options_config|live_settings|input_config"` PASS.

### Task 5: Конвейер геймпада в NativeInput

**Files:**
- Modify: `runtime/native_input.h/.cpp`
- Test: `tests/test_input.cpp` (или новый `tests/test_native_pad.cpp`, регистрация в `tests/CMakeLists.txt`)

**Interfaces:**
- Consumes: `GamepadSettings`, `stick_aim_velocity`, `camera_axes`, `gyro_aim_counts`, `GyroBias`, `merge_bindings` (Tasks 1, 2, 4).
- Produces:
```cpp
enum class PadSide:uint8_t { left,right };
void NativeInput::pad_settings(const GamepadSettings&);   // роли, кривые, гироскоп, таблица привязок (резолвер получает merge_bindings(клавиатура, pad.bindings))
void NativeInput::host_pad_stick(PadSide,float x,float y);   // -1..1, y вниз положительная (как SDL)
void NativeInput::host_pad_gyro(float yaw_rate,float pitch_rate,uint64_t timestamp_ns);   // рад/с
void NativeInput::pad_reset();   // отключение/потеря фокуса: стики в ноль, кнопки и оси геймпада отпущены
```
- Расширить `HostEventKind` значениями `pad_button`, `pad_axis` (`value` -32768..32767 как SDL), `pad_stick` (code 0 левый, 1 правый, `x`,`y`), `pad_gyro` (`x`,`y`) и валидацию в `apply_test_event`.
- Поведение: `bindings(const Bindings&)` хранит клавиатурную таблицу и пересобирает объединение; указатель при `consume()` сдвигается на `velocity*dt*высота` (dt по тому же времени, что `note_aim`, не больше 0,1 с; в закрытом меню и без фокуса скорость 0); стик с ролью камеры пишет `ActionFrame::camera` (клавиатура и стик складываются с клэмпом); `Action::recenter` ставит указатель в центр; гироскоп двигает указатель через ту же ветку, что `move_raw`, при режиме «всегда» или «hold» с зажатой `gyro_hold`, и питает детекторы ножа/встряски отсчётами `gyro_aim_counts` (при режиме не «Выкл»); входы `pad_axis` через `host_button` как кнопки; стики и курки при развороте на 50% (гистерезис 40%) отдаёт источник Task 6, а не `NativeInput`.

- [ ] **Step 1: Тесты:** `left_stick_moves_pointer_per_second` (стик (1,0), два `consume()` с шагом 0,5 с виртуальных часов даёт смещение `speed*0.5` высоты экрана с допуском); `dt_is_capped` (пауза 5 с даёт сдвиг не более 0,1*speed); `menu_context_stops_pointer`; `right_stick_writes_analog_camera` (`host_pad_stick(right,.5,0)` → камера `{~64,0}` после мёртвой зоны 10%); `swapped_roles_work`; `two_idle_sticks_with_drift_below_dead_zone_do_nothing` (дрейф 0,08 на обоих стиках, указатель и камера неподвижны); `recenter_action_centers_pointer`; `gyro_off_does_not_move`; `gyro_always_moves_and_hold_needs_button`; `desk_noise_below_dead_zone_does_not_move_after_calibration`; `gyro_swing_triggers_knife`; `gyro_shake_triggers_qte_shake` (в окне QTE); `pad_reset_releases_fire_and_zeroes_sticks`; `pad_events_via_test_command_are_acknowledged` (`apply_test_event` для новых видов, неверные коды отклоняются).
- [ ] **Step 2:** FAIL.
- [ ] **Step 3: Реализация**; интеграция скорости под тем же mutex, что и состояние указателя; не записывать в демо ничего нового сверх `sample` и `camera` (демо хранит итоговый `sample`).
- [ ] **Step 4:** PASS; прогнать весь набор `ctest --test-dir build -R input`.

### Task 6: Источник SDL и подключение в игре

**Files:**
- Create: `game/gamepad_input.h`, `game/gamepad_input.cpp`, `tests/test_gamepad_input.cpp`
- Modify: `game/uc_main.cpp` (`SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)` на `:175`, события в цикле около `:990`, `LiveSettings` цель `gamepad`, старт: `input.pad_settings(options.gamepad)`), `game/CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
```cpp
struct PadStatus { std::string name; bool connected=false,has_gyro=false; PadFamily family=PadFamily::generic; };
class GamepadInput {
public:
    bool event(const SDL_Event&,NativeInput&,bool menu_open);   // GAMEPAD_ADDED/REMOVED, BUTTON_DOWN/UP, AXIS_MOTION, SENSOR_UPDATE; true = событие поглощено
    void update(NativeInput&);    // раз в кадр: стики → host_pad_stick, оси (триггеры и стики) → host_button(pad_axis) на 50%/40%
    PadStatus status() const;     // последний использованный геймпад
    void focus_lost(NativeInput&);   // pad_reset
};
PadFamily pad_family_from_type(SDL_GamepadType);   // XBOX360/XBOXONE → xbox; PS3/PS4/PS5 → playstation; SWITCH_PRO/JOYCON → nintendo; прочее → generic
```
- Поведение: при подключении включается `SDL_SetGamepadSensorEnabled(SDL_SENSOR_GYRO,true)` если есть; отключение любого геймпада вызывает `pad_reset` для его входов; события геймпада в открытом меню не доходят до `NativeInput` игрового указателя (кнопки и оси идут только в захват привязок, см. Task 8); `SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS` не включается.

- [ ] **Step 1: Тесты** (SDL виртуальный геймпад без окна: `SDL_Init(SDL_INIT_GAMEPAD)`, `SDL_AttachVirtualJoystick` с типом GAMEPAD, 6 осей, 15 кнопок, сенсор гироскопа): `virtual_pad_button_reaches_native_input` (south down → действие interact в gameplay-контексте); `right_trigger_fires_at_half_pull` и гистерезис; `left_stick_event_sets_pointer_velocity`; `gyro_sensor_update_feeds_native_input`; `detach_releases_held_fire`; `family_from_type_table` (по таблице типов выше); `menu_open_does_not_feed_game_input`.
- [ ] **Step 2:** FAIL.
- [ ] **Step 3: Реализация**; один `GamepadInput` в `uc_main`, `focus_lost` вызывается там же, где `input.focus(false)`.
- [ ] **Step 4:** PASS. Реальный геймпад проверяет владелец (в плане не проверяется: нет железа у CI).

### Task 7: Подсказки: модель, глифы, устройство

**Files:**
- Modify: `common/input_prompts.h/.cpp`, `game/prompt_hooks.h/.cpp` (`prompt_hooks_set_bindings` и новая `prompt_hooks_set_device`, вызовы `uc_main.cpp:504`, `:830`), `renderer/prompt_draw.h/.cpp` (`gx::PromptDraw` несёт `pad_family`/`gyro`), `renderer/prompt_picture.cpp` (ветка рисования), `renderer/prompt_icons.h/.cpp`
- Create: `renderer/pad_glyphs.h`, `renderer/pad_glyphs.cpp`
- Test: `tests/test_input_prompts.cpp`, `tests/test_pad_glyphs.cpp`

**Interfaces:**
- Produces (`common/input_prompts.h`):
```cpp
struct PromptDevice { bool gamepad=false; PadFamily family=PadFamily::generic; bool gyro=false; };
PromptDescription primary_prompt(const Bindings&,std::initializer_list<Action>,const PromptDevice& = {});
PromptDescription chord_prompt(const Bindings&,Action first,Action second,const PromptDevice& = {});
std::string_view pad_glyph_label(PhysicalInput,PadFamily);   // xbox south "A", east "B", west "X", north "Y", LB "LB", RT "RT", Start "Menu", Back "View"; playstation south "Cross", east "Circle", west "Square", north "Triangle", LB "L1", RT "R2", Start "Options", Back "Create"; nintendo south "B", east "A", west "Y", north "X", LB "L", LT "ZL", RT "ZR", Start "+", Back "-"; generic "South","East","West","North","LB","RB","LT","RT","Start","Back"
```
`PromptDescription` получает поле `PadFamily pad_family=PadFamily::generic;` и `bool gyro=false;`; при `device.gyro` для `knife` и `qte_shake` альтернатива становится жестом (`PromptGesture::directional_motion` / `shake`).
- Produces (`renderer/pad_glyphs.h`): `void draw_pad_glyph(ImDrawList&,ImVec2 position,ImVec2 size,PhysicalInput,PadFamily);` и `void draw_gyro_icon(ImDrawList&,ImVec2 position,ImVec2 size,PadFamily);` рисуют процедурно в оригинальном стиле семейства (Xbox: цветные буквы A зелёная, B красная, X синяя, Y жёлтая на круглых кнопках; PlayStation: тёмные круглые кнопки с символами синий крест, красный круг, розовый квадрат, зелёный треугольник; Nintendo: светлые круглые кнопки с чёрными буквами; generic: нейтральные), `PromptStyle` не влияет; плечевые кнопки и курки как скруглённые прямоугольники с подписью, стики и D-pad как круги/крест, Start/Back как малые значки.
- В `uc_main.cpp` тип «Авто» выбирает устройство по последнему использованному (клавиша/мышь против геймпада, фиксируется в `HostInput`/`GamepadInput`), «Геймпад»/«Клавиатура и мышь» фиксируют; модель «Авто» берёт `GamepadInput::status().family`. Смена применяется через очередь `pending` хуков (как смена привязок).

- [ ] **Step 1: Тесты:** `pad_labels_match_family` по таблице выше; `primary_prompt_uses_pad_table_with_device` (таблица `default_pad_bindings()`, `PromptDevice{true,xbox}` → `reload` показывает west; `pad_family==xbox`); `gyro_device_turns_knife_and_shake_into_gestures`; `keyboard_prompts_unchanged_without_device` (результаты старых тестов не меняются); `pad_glyphs_draw_nonempty_for_every_family_and_binding` (для каждого семейства и каждого кода кнопки/оси таблицы умолчаний `ImDrawList` получил команды, без исключений).
- [ ] **Step 2:** FAIL.
- [ ] **Step 3: Реализация**; инвентарь мест подсказок взять из `docs/spikes/I3-prompt-boundaries.md` и `game/prompt_hooks.cpp` (`primary_prompt` `:76`, `:106`, `chord_prompt` `:107`: все три получают `PromptDevice`); внутриигровые подсказки, QTE и туториалы идут через эти хуки.
- [ ] **Step 4:** `ctest -R "input_prompts|pad_glyphs"` PASS; затем сборка `build.bat --no-install` целиком, проверить, что рендереры Vulkan/D3D11/D3D12 собрались.

### Task 8: Вкладка «Контроллеры», модальное окно, редактор привязок

**Files:**
- Modify: `launcher/binding_editor.h/.cpp` (конструктор `BindingEditor(Bindings bindings,Bindings defaults,DeviceTable table)` где `enum class DeviceTable{keyboard,gamepad}` выбирает проверку `validate_bindings` или `validate_pad_bindings` и умолчания `restore_defaults`; прежний конструктор ведёт себя как раньше), `launcher/main.cpp` (`sections[]` `:511` добавить `controllers`, `kNavigationTabs`; цикл событий `:308-318` кормит второй редактор входами геймпада, SDL_INIT_GAMEPAD на `:78`; сохранение `:282`, `:1073`), `game/settings_menu.h/.cpp` (вкладка `menu_controllers` после `menu_controls` на `:65-70`; `capture_event` `:35-40` принимает события геймпада), `launcher/translations.inc`, `launcher/CMakeLists.txt`, `game/CMakeLists.txt`
- Create: `launcher/gamepad_panel.h`, `launcher/gamepad_panel.cpp`
- Test: `tests/test_binding_editor.cpp`, `tests/test_settings_menu.cpp`, тест локализации (существующий для `translations.inc`)

**Interfaces:**
- Produces (`launcher/gamepad_panel.h`):
```cpp
using TrFn=std::function<const char*(std::string_view)>;
struct GamepadPanelState { bool modal_open=false; };
// Рисует содержимое вкладки «Контроллеры»: «Тип управления», «Модель геймпада», кнопка «Gamepad»; при modal_open модальное окно: имя геймпада и признак гироскопа, роли стиков, таблица действий с захватом, мёртвые зоны, скорость прицела, гироскоп (режим, кнопка, чувствительность, инверсия), «Сбросить по умолчанию». Возвращает true, когда настройки изменились.
bool gamepad_tab(GamepadPanelState&,GamepadSettings&,BindingEditor& pad_editor,const PadStatus&,float unit,const TrFn&);
```
- Названия кнопок в таблице берутся из `pad_glyph_label(input, family)` (семейство выбранной модели или подключённого геймпада).
- Все ключи локализации (`menu_controllers`, `gamepad`, `control_type`, `control_auto`, `control_keyboard_mouse`, `control_gamepad`, `pad_model`, `stick_aim`, `stick_camera`, `dead_zone`, `aim_speed`, `gyro`, `gyro_mode_off`/`always`/`hold`, `gyro_sensitivity`, `invert`, `no_pad`, `recenter`) добавить строками `translations.inc` в формате существующих рядов (EN и RU обязательны; прочие столбцы по соглашению файла для неперведённого текста, проверить тестом локализации).
- В игре изменения применяются сразу через `LiveSettings` цель `gamepad` (Task 4); в лаунчере сохраняются вместе с остальными настройками.

- [ ] **Step 1: Тесты:** `pad_editor_captures_pad_button_and_axis` (`event({pad_button,0},true)` в режиме захвата добавляет привязку, клавиша отклоняется); `pad_editor_restore_defaults_gives_pad_defaults`; `keyboard_editor_unchanged`; `settings_menu_has_controllers_tab_and_keeps_controls_keyboard_only` (в `exercise`: вкладка «Контроллеры» присутствует, вкладка «Управление» не содержит строк геймпада); `gamepad_tab_changes_model_and_reports_changed`; `capture_modal_accepts_pad_event`; проверка локализации: все новые ключи есть в EN и RU.
- [ ] **Step 2:** FAIL.
- [ ] **Step 3: Реализация**; модальное окно через `ImGui::OpenPopup`/`BeginPopupModal`; событие захвата идёт только пока редактор в режиме захвата.
- [ ] **Step 4:** `ctest -R "binding_editor|settings_menu|locale"` PASS. Затем визуальная проверка по правилу web-verify недоступна (нативный ImGui): проверить через harness-скриншот окна меню 1280x720 (`menu_click` в тестовом сервере), одна проверка вкладки в игре и одна в лаунчере.

### Task 9: Сценарий харнесса и документация

**Files:**
- Modify: `game/test_protocol.h` (`TestDevice` добавить `pad_button`, `pad_axis`, `pad_stick`, `pad_gyro`), `game/test_server.cpp:45-51`, `game/uc_main.cpp:924-930`, `scripts/uctest/client.py` (хелперы `pad_button`, `pad_stick`, `pad_gyro`), `scripts/uctest/scenarios.py` (сценарий `gamepad`), `docs/testing/test-server.md:25`, `docs/tech-debt.md` (если остаётся осознанный shortcut, например «режим геймпада плашмя не реализован»)
- Test: `tests/test_test_protocol.cpp` (если есть; иначе расширить существующий тест разбора запросов)

**Interfaces:**
- Consumes: `HostEventKind::pad_*` из Task 5.
- Produces: JSON-команды `{"cmd":"input","pad_button":{"button":0,"down":true}}`, `{"pad_stick":{"side":"left","x":1,"y":0}}`, `{"pad_gyro":{"x":0,"y":0.5}}`; сценарий `uctest run gamepad`: загрузка до игрового состояния, левый стик сдвигает прицел (`state` показывает смещение), RT стреляет (счётчик патронов уменьшается), правый стик двигает камеру аналогово (разные отклонения дают разные `stick_x`), `recenter` ставит прицел в центр.

- [ ] **Step 1: Тесты** разбора новых запросов (допустимые и недопустимые поля).
- [ ] **Step 2:** FAIL.
- [ ] **Step 3: Реализация** и сценарий `gamepad`; обновить таблицу в `docs/testing/test-server.md`.
- [ ] **Step 4:** `ctest -R test_protocol` PASS; один прогон `cd scripts && python -m uctest run gamepad --speed 8` (окно 1280x720, без фуллскрина), ожидание: сценарий проходит; результат записать в итог.

### Task 10: Полная проверка и выкладка

- [ ] **Step 1:** `ctest --test-dir build --output-on-failure` целиком после `build.bat --no-install`. Ожидание: все тесты зелёные.
- [ ] **Step 2:** Регрессия демо: `python -m uctest demo check fight4` (старое демо воспроизводится после смены формата). Ожидание: replay равен recording.
- [ ] **Step 3:** Финальное ревью ветки (`superpowers:requesting-code-review`, субагент с `model: "opus"`).
- [ ] **Step 4:** Полная сборка с выкладкой: из PowerShell `& "D:\Projects\Native Ports\Umbrella-chronicles-recomp\build.bat"` (без `--no-install`); перед этим удалить из `dist` отладочные дампы и тестовые файлы. Сообщить путь и время выкладки.
- [ ] **Step 5:** Памятку в `MEMORY.md` о принятых решениях (аналоговая камера, версия демо/сети 2) добавить только по запросу владельца.
