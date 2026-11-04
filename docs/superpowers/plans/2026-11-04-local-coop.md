# Игра вдвоём на одном компьютере: план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Двое играют на одном компьютере (Windows и Linux на компьютере) с любыми сочетаниями устройств; второй игрок присоединяется нажатием кнопки на другом устройстве.

**Architecture:** Движок уже принимает два `NativeInput` (`input_hle_init(&a,&b)`, `game_input_init(&a,trace,&b)`, как в `game/netplay_game.cpp`). Добавляем: чистый автомат присоединения (`CoopRoster`), второй `NativeInput` и второй `HostInput` в `game/uc_main.cpp`, разводку геймпадов и Wii Remote по игрокам, слой «устройство мыши/пушки» (evdev на Linux, Raw Input на Windows) и Wii Remote на Linux поверх evdev-узлов `hid-wiimote`, переложенных в уже существующий протокольный слой (`WiiTransport`).

**Tech Stack:** C++20, SDL3, CMake, ImGui (меню), evdev (Linux), Raw Input (Windows).

**Spec:** `docs/superpowers/specs/2026-11-04-local-coop-design.md`

## Global Constraints

- Платформы: Windows и Linux на компьютере (`--ui desktop`). Steam Deck (`deck_view`, меню `DeckMenu`, `--ui deck`, Game Mode) не меняется: присоединение там выключено.
- Нет переключателя режима: P2 появляется нажатием кнопки на устройстве, которое P1 не использовал; движение не присоединяет.
- Присоединение только во время игры; в меню и на паузе не срабатывает; нажатие в меню отмечает устройство как P1.
- Клавиатура всегда у P1; клавиатуры как устройства P2 нет.
- Привязки P2 по умолчанию как у устройства; переназначение P2 как у P1 (клавиатура+мышь, геймпад), кроме Wii Remote: второй пульт использует `wiimote.bindings` P1.
- Сеть, демо, `bench`: P2 не присоединяется (кроме событий тестового сервера). Без P2 всё как было, бит в бит.
- Linux: порт читает только указывающие устройства `/dev/input` (без клавиатур), открывает на чтение без захвата, ничего не пишет и не логирует; `EVIOCGRAB` только у присоединённого устройства P2; снимается при уходе и выходе.
- Источники правды по тексту: UI копирует существующие окна (`BindingEditor`, правило единого стиля); проверка на минимальном размере окна.
- Правки исходников только через Edit/Write; после правок со строковыми escape проверить строку Grep'ом.
- Коммиты и сборка с деплоем только по просьбе владельца (деплой после завершения всей задачи: `build.bat` без `--no-install`).

## Review Focus

- Движение мыши без нажатия не присоединяет P2 (тест в Task 1 и Task 6).
- Два устройства нажали кнопку в одном кадре: P2 один, второй остаётся без роли (Task 1).
- Устройство P2 отключено посреди игры: захват снят, зажатые кнопки отпущены, игра не зависает (Task 8, 9).
- Нажатие на устройстве P2 на паузе и в меню не присоединяет и не пропускает ввод в игру (Task 9).
- Две абсолютные пушки с одинаковыми координатами и NaN/выход за диапазон оси не ломают указатель (Task 6, 7).
- После «Отключить второго игрока» старое устройство не присоединяется само (Task 1).
- Нет прав на `/dev/input`: игра идёт, в меню причина (Task 7, 10).
- Сеть или демо активны: P2 не присоединяется (Task 9).

---

### Task 1: Автомат присоединения `CoopRoster`

**Files:**
- Create: `common/coop_roster.h`, `common/coop_roster.cpp`
- Modify: `common/CMakeLists.txt` (библиотека `uc_coop_roster`)
- Test: `tests/test_coop_roster.cpp` (+ строки в `tests/CMakeLists.txt` по образцу `test_input_config`)

**Interfaces:**
- Produces:
```cpp
namespace uc {
enum class DeviceKind:uint8_t { keyboard,pointer,gamepad,wiimote };   // pointer: мышь или lightgun
struct DeviceId { DeviceKind kind; uint64_t id; bool operator==(const DeviceId&) const=default; };   // id: SDL_JoystickID, путь/идентификатор evdev или HID, handle Raw Input
enum class Player:uint8_t { one,two };
class CoopRoster {
public:
    // A press (never motion) on `device`. Returns Player::two exactly when this press joined the second player.
    std::optional<Player> press(DeviceId device,bool in_gameplay);
    Player route(DeviceId device) const;                 // every device but P2's goes to P1
    std::optional<DeviceId> second() const;              // P2's device
    bool remove(DeviceId device);                        // unplugged; true when it was P2's (P2 is gone)
    void release_second();                               // menu command: the device keeps feeding P1 and cannot join again
};
}
```

- [ ] **Step 1: Write the failing tests** in `tests/test_coop_roster.cpp`: keyboard press never joins; gamepad pressed in a menu is P1's and later pressed in gameplay does not join; an unused gamepad pressed in gameplay joins (`press` returns `Player::two`, `second()` equals it, `route` returns `Player::two`); a second unused device pressed afterwards in gameplay returns `Player::one` (becomes P1's) while P2 exists; a press with `in_gameplay=false` on an unused device marks it P1's and does not join; `remove` of P2's device returns true and clears `second()`; after `remove` a new unused device joins; `release_second()` leaves the former device routed to P1 and a later press on it does not rejoin; two presses in the same call order give exactly one P2.
- [ ] **Step 2: Run to verify it fails.** `cmake --build build --target test_coop_roster` then `scripts/ctest-hidden.ps1 -R test_coop_roster --output-on-failure`. Expected: build error (missing header).
- [ ] **Step 3: Implement `CoopRoster`** in `common/coop_roster.cpp` as a small set of `DeviceId` (P1-owned) plus an optional P2 device; no I/O.
- [ ] **Step 4: Run tests.** Expected: PASS.
- [ ] **Step 5: Commit** (only if the owner asked): `git add common/coop_roster.* tests/test_coop_roster.cpp tests/CMakeLists.txt common/CMakeLists.txt`

### Task 2: Настройки P2 в `config.json`

**Files:**
- Create: `common/coop_config.h`, `common/coop_config.cpp`
- Modify: `game/options.h` (поле `CoopSettings player2;`), `game/live_settings.h` and `game/live_settings.cpp` (цель `std::function<void(const CoopSettings&)> player2;` по образцу `wiimote`), `game/CMakeLists.txt` / `common/CMakeLists.txt`
- Test: `tests/test_coop_config.cpp`

**Interfaces:**
- Consumes: `GamepadSettings`, `Bindings`, `validate_bindings`, `validate_gamepad` (`common/gamepad_config.h`, `common/input_bindings.h`).
- Produces:
```cpp
struct CoopSettings {
    Bindings bindings=default_bindings();     // keyboard-and-mouse table of P2: only the mouse part is ever used (no second keyboard)
    GamepadSettings gamepad;                  // P2's gamepad: its own table and sticks; defaults as P1's
    bool operator==(const CoopSettings&) const=default;
};
std::optional<std::string> validate_coop(const CoopSettings&);
CoopSettings coop_from_config(const ConfigDocument&);      // "player2": {"bindings":{...},"gamepad":{...}}; absent = defaults; malformed throws std::invalid_argument("config: player2: ...")
void store_coop(ConfigDocument&,const CoopSettings&);      // throws and leaves the document alone when invalid
```

- [ ] **Step 1: Write the failing tests:** absent object gives defaults equal to `CoopSettings{}`; round trip through `store_coop` then `coop_from_config` keeps a changed gamepad binding and a changed mouse binding; unknown key refused; invalid table throws and document unchanged.
- [ ] **Step 2: Run, expect FAIL** (`test_coop_config`).
- [ ] **Step 3: Implement** reusing `bindings_from_json`/`bindings_to_json` and the gamepad parsing of `gamepad_config.cpp` (factor a shared function there only if needed; no copy of its code).
- [ ] **Step 4: Run, expect PASS.** Also run `test_input_config` to see nothing else changed.
- [ ] **Step 5: Hook `options.player2 = coop_from_config(doc)` where `options.gamepad` is loaded** (`grep gamepad_from_config game/*.cpp`).

### Task 3: Второй игрок в движке (`NativeInput` второго канала и признак подключения)

**Files:**
- Modify: `runtime/native_input.h`, `runtime/native_input.cpp` (`hle_WPADProbe`), `game/uc_main.cpp` (создание `input2`, `uc::input_hle_init(&input,&input2)` и `uc::game_input_init(...,&input2)` рядом со строкой `game_input_init(options.headless ? nullptr : &input,...)`)
- Test: `tests/test_input.cpp`, `tests/test_game_input.cpp` (около строки 293, где уже есть `second`)

**Interfaces:**
- Produces: `void input_hle_set_second_connected(bool)` in namespace `uc` (default false; `input_hle_init` resets it to true only for network play, see below); `hle_WPADProbe(channel 1)` reports connected exactly when a second input exists **and** the flag is set.

- [ ] **Step 1: Write the failing tests:** with `input_hle_init(&a,&b)` and the flag false, `hle_WPADProbe` for channel 1 returns -1 and type `0xFD`; after `input_hle_set_second_connected(true)` it returns 0 and type 1; `hle_KPADRead(channel 1)` returns `b`'s sample, channel 0 returns `a`'s. Network play (`NetGame`) must keep channel 1 connected: assert `input_hle_init` called by `NetGame` path leaves it connected (add `input_hle_set_second_connected(true)` next to `input_hle_init(&impl_->pads[0],&impl_->pads[1])` in `game/netplay_game.cpp:678`).
- [ ] **Step 2: Run, expect FAIL.** Step 3: implement; the local default in `uc_main.cpp` is `input_hle_set_second_connected(false)`. Step 4: PASS plus full `test_input`, `test_game_input`, `test_netplay*`.
- [ ] **Step 5: Spike (record result in this task's commit message and in the spec's "Оставлено сборщику"):** run the harness (`uctest`, 1280x720, see `.claude/skills/uc-test/SKILL.md`) with `input2` connected and a button on channel 1: does the game take a second player with only the channel connected, or does it need its own joining press? Record what P2 must press and, if the game needs it, make Task 9 pass the joining press through to `input2`.

### Task 4: Геймпады по игрокам

**Files:**
- Modify: `game/gamepad_input.h`, `game/gamepad_input.cpp`
- Test: `tests/test_gamepad_input.cpp`

**Interfaces:**
- Produces: `GamepadInput::event(const SDL_Event&,GamepadTarget* first,GamepadTarget* second=nullptr)`, `GamepadInput::update(GamepadTarget& first,bool active,GamepadTarget* second=nullptr)`, `void GamepadInput::assign_second(SDL_JoystickID)` (0 = none), `std::optional<SDL_JoystickID> GamepadInput::last_pressed() const` (id of the pad with the newest button press, for the roster). The pad with the assigned id feeds `second`, all others feed `first`. Rumble: `rumble(motors,...)` bit 1 (player 2) drives the assigned pad when there is one, else as today.

- [ ] **Step 1: Write failing tests** using the virtual SDL joysticks the file already uses: two pads, pad B assigned; a button of B reaches only the second target; sticks of B reach only the second target's `host_pad_stick`; unassigned behaviour equals today's; `assign_second(0)` returns B's input to `first`; removing B (`SDL_EVENT_GAMEPAD_REMOVED`) calls `pad_reset` on the second target.
- [ ] **Step 2: FAIL. Step 3: implement. Step 4: PASS** and the existing tests in the file still pass.

### Task 5: Два Wii Remote на Windows и подготовка протокольного слоя

**Files:**
- Modify: `game/wiimote_hid.cpp`, `game/wiimote_transport.h`, `game/wiimote_input.h`, `game/wiimote_input.cpp`, `common/wiimote_protocol.h`, `common/wiimote_protocol.cpp`
- Test: `tests/test_wiimote_input.cpp`, `tests/test_wiimote_protocol.cpp` (add if missing; follow the file that tests `wii_parse_report`)

**Interfaces:**
- Produces:
  - `std::unique_ptr<WiiTransport> wii_open_unclaimed(std::set<std::string>& claimed,std::string& error);` in `wiimote_transport.h`: opens the first remote whose HID path is not in `claimed`, adds the path, removes it when the transport is destroyed. `wii_open_first` stays and equals `wii_open_unclaimed` with a throw-away set.
  - `std::string WiiTransport::id() const` (virtual, default empty): stable device identifier (the HID path or evdev unique string).
  - `std::array<uint8_t,kWiiInputBytes> wii_encode_report(const WiiState&)`: builds the 0x37 report (buttons, accelerometer, basic IR, extension bytes) that `wii_parse_report` turns back into the same `WiiState`.
  - `bool WiimoteInput::pending_press() const` (main thread): a button-down event is queued and `update` has not handed it on yet.
  - `std::string WiimoteInput::device_id() const` (the connected transport's `id()`, empty without connection).
  - `void WiimoteInput::set_player(unsigned player)` (0 or 1): the LED the handshake and the status request light (`wii_led_report(player+1)`); sent again when changed while connected.

- [ ] **Step 1: Write failing tests:** `wii_encode_report` then `wii_parse_report` round trip for buttons (all 13 bits), accel, a four-dot IR set and a nunchuk stick; a fake `WiiTransport` pair: two `WiimoteInput` objects given openers that share one `claimed` set open different fake devices; `pending_press` is true after a queued press and false after `update`; `set_player(1)` makes the next written LED report equal `wii_led_report(2)`.
- [ ] **Step 2: FAIL. Step 3: implement** (`wii_open_unclaimed` in `wiimote_hid.cpp` enumerates HID paths as `wii_open_first` does today, skipping claimed ones). **Step 4: PASS**, full `test_wiimote_input`.

### Task 6: Слой устройств мыши и пушек (общая часть)

**Files:**
- Create: `common/pointer_devices.h`, `common/pointer_devices.cpp`
- Modify: `common/CMakeLists.txt`
- Test: `tests/test_pointer_devices.cpp`

**Interfaces:**
- Produces:
```cpp
struct PointerEvent {
    enum class Kind:uint8_t { button,relative,absolute } kind;
    uint64_t device;            // DeviceId::id of a DeviceKind::pointer
    uint8_t button=0;           // SDL_BUTTON_* numbering (1 left, 2 middle, 3 right, 4 x1, 5 x2)
    bool down=false;
    float x=0,y=0;              // relative: counts; absolute: 0..1 of the window (y down)
};
class PointerBackend {          // one per platform; tests bring a fake
public:
    virtual ~PointerBackend()=default;
    virtual void poll(std::vector<PointerEvent>& out)=0;       // everything that arrived since the last call, in order
    virtual bool capture(uint64_t device,bool on)=0;           // keep the device away from the OS and SDL (EVIOCGRAB on Linux; Windows backends swallow its messages); false when it cannot
    virtual std::string status() const=0;                      // "" when well; why devices cannot be read (shown in the menu)
};
// A PointerEvent as the SDL mouse event HostInput already understands (window pixels for absolute: x*width, y*height divided by density).
SDL_Event pointer_event_to_sdl(const PointerEvent&,SDL_WindowID,uint32_t width,uint32_t height,float density);
```
  - `PointerEvent` values that are NaN, infinite or outside 0..1 for absolute are dropped by `pointer_event_to_sdl` (it returns an event of type `SDL_EVENT_USER`).

- [ ] **Step 1: Write failing tests:** relative event gives `SDL_EVENT_MOUSE_MOTION` with `xrel,yrel`; absolute 0.5,0.5 in 1280x720 at density 1 gives `x=640,y=360`, at density 2 gives 320,180 for a 2560x1440 window; NaN and 1.5 are dropped; button event gives `SDL_EVENT_MOUSE_BUTTON_DOWN/UP` with the right `button`. Feeding the produced events to a `HostInput` + `NativeInput` pair (as `test_host_input.cpp` does) moves the aim: relative moves it, absolute puts it under the position (use the same assertions style as that file).
- [ ] **Step 2: FAIL. Step 3: implement. Step 4: PASS.**

### Task 7: Linux: evdev для мыши и пушек и для Wii Remote

**Files:**
- Create: `common/evdev_logic.h`, `common/evdev_logic.cpp` (чистая логика, собирается и тестируется на Windows), `game/evdev_backend.cpp` (Linux, fd-слой: `open`, `read`, `ioctl`, перечисление `/dev/input`, повторный просмотр каждые 2 с, `EVIOCGRAB`), `game/wiimote_evdev.cpp` (Linux: `wii_open_first`, `wii_open_unclaimed`, транспорт `EvdevWiiTransport`)
- Modify: `game/CMakeLists.txt`: `uc_wiimote_input` uses `wiimote_evdev.cpp` on non-Windows instead of `wiimote_hid_stub.cpp` (delete the stub); `uc_pointer_evdev` library only for `UNIX`
- Test: `tests/test_evdev_logic.cpp`

**Interfaces:**
- Produces (in `evdev_logic.h`, no Linux headers; the few constants `EV_KEY=1,EV_REL=2,EV_ABS=3,REL_X=0,REL_Y=1,ABS_X=0,ABS_Y=1,BTN_LEFT=0x110,BTN_TOUCH=0x14a,BTN_TOOL_FINGER=0x145` are defined locally):
```cpp
struct EvdevEvent { uint16_t type,code; int32_t value; };
struct EvdevCaps { bool rel_xy,abs_xy,left_button,touch,tool_finger,keyboard_keys; int abs_min[2],abs_max[2]; std::string name,unique; };
enum class EvdevRole:uint8_t { ignore,mouse,gun,wiimote_buttons,wiimote_accel,wiimote_ir,wiimote_nunchuk };
EvdevRole classify_evdev(const EvdevCaps&);   // mouse: rel_xy+left_button; gun: abs_xy+(left_button|touch) without tool_finger (touchpads and tablets with tool_finger are ignored); keyboards ignored; "Nintendo Wii Remote*" names by suffix
void feed_pointer(PointerState&,EvdevCaps const&,std::span<const EvdevEvent>,uint64_t device,std::vector<PointerEvent>& out);   // EV_SYN-delimited; relative sums per frame; absolute normalises by abs_min/abs_max; BTN_LEFT/RIGHT/MIDDLE/SIDE/EXTRA map to SDL buttons 1/3/2/4/5
WiiState feed_wiimote(WiiState,EvdevRole,std::span<const EvdevEvent>);   // node events into the shared WiiState (buttons, accel, IR dots, nunchuk stick)
```
  - `EvdevWiiTransport` groups the four evdev nodes of one remote by `unique` (the Bluetooth address), implements `WiiTransport`: `read` returns `wii_encode_report(state)` when any node delivered events or after a 10 ms timeout; `write` handles output reports: LED report 0x11 (to `/sys/class/leds/<unique-based>::player<N>` brightness), rumble bit 0 (force feedback `EVIOCSFF` on the button node), the status request 0x15 (queues a status report 0x20 with the extension flag from the presence of the nunchuk node); every other report (extension and IR setup) is accepted and ignored, because `hid-wiimote` does that itself. `id()` returns `unique`.
  - Constants and axis ranges of the `hid-wiimote` nodes (button KEY codes, accelerometer ABS_RX/RY/RZ range, IR ABS_HAT0X..3Y, nunchuk stick range) must be copied from the kernel source `drivers/hid/hid-wiimote-modules.c` into `evdev_logic.cpp` as named constants with a comment naming that file; do this first and write the test values from it.

- [ ] **Step 1: Write failing tests:** classification table (optical mouse, Sinden-like absolute gun, touchpad with `tool_finger` ignored, keyboard ignored, each Wii Remote node by name); `feed_pointer` relative frame sum, absolute normalisation with a 0..32767 range and a range with nonzero minimum, out-of-range value clamped, `BTN_*` mapping; `feed_wiimote` buttons set/clear the right `WiiButton` bits, accel scaled to the 8-bit `WiiState::accel`, IR dots valid when inside 0..1023 x 0..767 and invalid when the kernel reports its "no point" value, nunchuk stick to 0..255; round trip `feed_wiimote` -> `wii_encode_report` -> `wii_parse_report`.
- [ ] **Step 2: FAIL. Step 3: implement `evdev_logic`; then the Linux fd layers** (`PointerBackend` implementation `EvdevPointerBackend` in `game/evdev_backend.cpp`: read-only open of pointer-like devices only, `status()` says "no access to /dev/input" when devices exist but cannot be opened; `capture(device,true)` does `EVIOCGRAB`, `capture(device,false)` releases; on read error the device is dropped and its held buttons are reported released).
- [ ] **Step 4: PASS** (`test_evdev_logic` on Windows). **Step 5: Linux check on the Steam Deck** over `ssh deck` with `uinput`: create a virtual mouse, a virtual absolute gun and virtual hid-wiimote-like nodes (same names and unique) with a short script kept in `scripts/linux/uinput_fakes.py`; verify the backend sees them, `capture` stops the events reaching SDL, unplugging releases the grab. Clean the Deck afterwards (owner rule: keep its disk clean).

### Task 8: Windows: мыши по устройству и абсолютные пушки

Проверено по исходнику SDL (`build/_deps/sdl3-src/src/video/windows/SDL_windowsevents.c`, `WIN_HandleRawMouseInput`): при игре одним игроком в относительном режиме (`pointer=relative`, умолчание Windows) порт уже читает мышь через Raw Input SDL, и каждое событие несёт `which` = handle устройства (`(SDL_MouseID)hDevice`). Поэтому:

- **Относительные мыши:** отдельный бэкенд не нужен. `DeviceId{pointer, which}` берётся из `event.motion.which` / `event.button.which`; события мыши P2 (`which == second`) идут в `second_host`, не в P1. Это делает `CoopSession::tick` (Task 9) по SDL-событиям; `PointerBackend` для таких мышей не создаётся.
- **Абсолютные пушки** (`pointer=absolute`: SDL берёт курсор окна и `SDL_GLOBAL_MOUSE_ID`; в относительном режиме SDL отбрасывает скачки абсолютных устройств больше 32 пикселей): нужен собственный `RawInputBackend`, как ниже. Он включается, только когда P2 присоединился с абсолютным устройством или P1 в режиме `absolute` и у P2 тоже указывающее устройство.

**Files:**
- Create: `game/raw_input_backend.cpp` (Windows), `common/raw_pointer_math.h` (чистая функция)
- Modify: `game/CMakeLists.txt`
- Test: `tests/test_raw_pointer_math.cpp`

**Interfaces:**
- Produces: `struct PixelRect { int left,top,width,height; };` and `std::array<float,2> raw_absolute_to_window(int32_t last_x,int32_t last_y,PixelRect desktop,PixelRect window_client)`: maps `MOUSE_MOVE_ABSOLUTE` values (0..65535 over the primary or virtual desktop) to 0..1 of the window client area, clamped. `RawInputBackend : PointerBackend` reads `WM_INPUT` in a hook set with `SDL_SetWindowsMessageHook` (no second `RegisterRawInputDevices`; the hook returns false for messages of captured devices so SDL never sees them); keyed by `RAWINPUT::header.hDevice`.
- Constraint: when P2 has a pointer device, P1's pointer must also come from this backend (the OS cursor is hidden and locked to the window), switched on only while P2 is a pointer; with P1 alone nothing changes.

- [ ] **Step 1: Write failing tests for `raw_absolute_to_window`:** corners of a primary-monitor window, a window on a second monitor with virtual-desktop flag, clamping outside the window, zero-size window gives {0.5,0.5}.
- [ ] **Step 2: FAIL. Step 3: implement the math, then the backend.** First verify that SDL3 delivers `WM_INPUT` to the hook in the modes used (relative and absolute pointer); if it does not, register Raw Input ourselves with `RIDEV_INPUTSINK` for the window while P2 is a pointer and feed P1 from it too (fallback named in the spec). Write the finding into the spec's "Оставлено сборщику".
- [ ] **Step 4: PASS the math test; build the Windows exe.**

### Task 9: Подключение в игровом цикле `uc_main.cpp`

**Files:**
- Modify: `game/uc_main.cpp`; create `game/coop_session.h` and `game/coop_session.cpp` (glue, so `uc_main.cpp` only constructs and calls it)
- Test: `tests/test_coop_session.cpp`

**Interfaces:**
- Consumes: `CoopRoster` (Task 1), `CoopSettings` (Task 2), `input_hle_set_second_connected` (Task 3), `GamepadInput::assign_second/last_pressed` (Task 4), `WiimoteInput::pending_press/device_id/set_player` (Task 5), `PointerBackend` and `pointer_event_to_sdl` (Task 6), `EvdevPointerBackend` / `RawInputBackend` (Tasks 7-8).
- Produces:
```cpp
class CoopSession {
public:
    struct Parts { NativeInput& first; NativeInput& second; HostInput& second_host; GamepadInput* gamepad; std::array<WiimoteInput*,2> remotes; PointerBackend* pointers; };
    CoopSession(Parts,bool enabled);   // enabled=false: deck_view, headless, network, demo record/play, controlled harness runs without a real pad: a no-op
    void tick(bool in_gameplay,bool paused_or_menu,uint32_t width,uint32_t height,float density,SDL_WindowID,uint64_t now_ns);
    struct Status { bool joined=false; std::string device,problem; };
    Status status() const;
    void release_second();            // menu command
};
```
`tick` per loop: poll pointer events and gamepad/remote presses into the roster; on `Player::two` assign the device (gamepad: `assign_second`; remote: its reader's target switches to `second` and `set_player(1)`; pointer: `capture(device,true)` and its events go to `second_host` via `pointer_event_to_sdl`), call `input_hle_set_second_connected(true)`; on device loss `remove`, release held buttons on `second` (`pad_reset` / `wii_reset`), release capture. A press is handed to the chosen target (P2's join press reaches `second` too if the Task 3 spike said the game needs it). `second` has `bindings(player2.bindings)`, `pad_settings(player2.gamepad)`, `wiimote_settings(options.wiimote)` (inherits P1's remote table). Rumble: player 2's motor goes to P2's pad or remote (`RumbleRouter` gets a second device).

- [ ] **Step 1: Write failing tests** with fakes (fake `PointerBackend`, fake `WiiTransport`s, SDL virtual joysticks): gamepad B pressed in gameplay joins and `input_hle` channel 1 reports connected; same press while `paused_or_menu` does not join; mouse motion alone does not join; two devices in one tick give one P2; fake pointer for P2 moves `second`'s aim and not `first`'s; `release_second()` then a press on B does not rejoin; unplugging B releases its held buttons and returns connected to the neutral state described in the spec (channel stays connected, input neutral); `enabled=false` never joins.
- [ ] **Step 2: FAIL. Step 3: implement and call `coop_session.tick(...)` from the loop next to `gamepad->update` / `wiimote->update`; disable for `deck_view`, `network`, demo options, `headless`.** **Step 4: PASS**, plus `test_input`, `test_game_input`, `test_netplay*`, `test_rumble_hold` unchanged.

### Task 10: Меню: «Игрок 2»

**Files:**
- Modify: `game/settings_menu.h`, `game/settings_menu.cpp` (вкладка «Контроллеры»), `game/live_settings.*`, переводы (найти по ключу строки вкладки: `grep -rn "pad_status\|gamepad_tab" game`)
- Test: существующие тесты меню и live settings (`tests/test_settings_menu*`, `tests/test_live_settings*`; найти по `grep -l settings_menu tests/*.cpp`)

**Interfaces:**
- Consumes: `CoopSession::status/release_second` via `settings_menu->set_coop(...)`; `CoopSettings` target `targets.player2` (Task 2).
- Produces: на вкладке «Контроллеры» переключатель «Игрок 1 / Игрок 2» над теми же окнами `BindingEditor` (клавиатура и мышь, геймпад); для Игрока 2 редакторы работают с `options.player2.bindings` / `options.player2.gamepad.bindings`, захват клавиш клавиатуры в редакторе P2 запрещён (клавиатуры у P2 нет), окно Wii Remote у P2 отсутствует с подписью, что пульт берёт привязки первого; строка состояния («ждёт нажатия кнопки на другом устройстве», устройство, «нет доступа к /dev/input») и кнопка «Отключить второго игрока». `DeckMenu` не меняется.

- [ ] **Step 1: Write failing tests:** live settings applies a changed `player2` through `targets.player2` and saves it; the menu with P2 selected edits the P2 table, not P1's; status text for each of not-joined, joined, no-access; release button calls the target once.
- [ ] **Step 2: FAIL. Step 3: implement reusing the existing editor widgets and style (find the nearest equivalent first).** **Step 4: PASS. Step 5: screenshot via the harness** (1280x720 and the smallest supported window) next to the existing gamepad tab; say what was compared.

### Task 11: Тестовый сервер, сценарий, документы, сборка

**Files:**
- Modify: `runtime/native_input.h` (`HostEvent` gets `uint8_t player=0`), `runtime/native_input.cpp` (`apply_test_event` routes to the second input when `player==1`), `game/uc_main.cpp` (harness hands events by player), `scripts/uctest/*` (+ new scenario `scripts/uctest/scenarios/coop.py` modelled on an existing scenario), `docs/testing/test-server.md`, `README.md`, `docs/local-coop.md`, `docs/local-coop.ru.md`
- Test: `tests/test_input.cpp`; harness run

- [ ] **Step 1: Failing tests:** a `HostEvent` with `player=1` moves only the second input; `player=0` unchanged.
- [ ] **Step 2: FAIL. Step 3: implement; scenario:** P1 and P2 move independent aims, both fire; the P2 join happens by a `pad_button` event with `player=1` in gameplay; run `cd scripts && python -m uctest run coop --speed 8` (window 1280x720, never fullscreen, never the real saves).
- [ ] **Step 4: PASS the scenario.** Docs: how a second player joins, which devices count, `/dev/input` access (group `input` or a udev rule), `hid-wiimote` and Bluetooth pairing, Dolphin Bar mode 4, what Deck does not get; remote-testing result of Task 7 step 5.
- [ ] **Step 5: Full `ctest` and `build.bat` (no `--no-install`, after cleaning test leftovers from `dist`) so the build is deployed to the game folder; state the path and time.** Live check with real Wii Remotes, Dolphin Bar and guns is left to players.

---

## Self-review

- Spec coverage: join rules (Task 1, 9), config and bindings (2, 10), engine second channel (3), gamepads (4), Wii Remote Windows and Linux (5, 7), mice and guns Linux and Windows (6, 7, 8), menu (10), rumble and LEDs (5, 9), network/demo/Deck exclusions (9), tests and docs (11), permissions (7, 10, 11).
- Open items kept for builders per the spec: how the game accepts P2 (Task 3 spike), `WM_INPUT` in the SDL hook (Task 8), evdev constants from the kernel source (Task 7), neutral-channel behaviour on P2 loss (Task 9 test and a check on the game).
