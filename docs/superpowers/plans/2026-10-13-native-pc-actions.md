# Native PC Actions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Owner selected Native; preserve this method.

**Goal:** Реализовать согласованные действия мыши/клавиатуры и общую модель переназначений, включая реальные игровые Reload/Grenade/Knife без поддельных motion-сэмплов.

**Architecture:** Общий C++ резолвер хранит типизированные бинды и обрабатывает упорядоченные host-события. SDL публикует события через существующий mutex NativeInput; игровой поток принимает их под scheduler baton. Проверенные game-level адаптеры запускают штатную логику игры, а дальнейшие этапы лаунчера и подсказок используют ту же модель.

**Tech Stack:** C++20, установленный clang-cl/MSVC SDK, SDL3.2.0, существующий AOT-translator/xref, CMake/Ninja/CTest. Новых зависимостей на этом этапе нет.

**Spec:** `docs/superpowers/specs/2026-10-13-native-pc-controls-design.md` — согласована пользователем2026-10-13. Основная спека: `docs/superpowers/specs/2026-10-12-uc-recomp-design.md`.

## Global Constraints

- ЛКМ Interact; ПКМ Fire; колесо вверх/W WeaponPrevious; колесо вниз/S WeaponNext; R Reload; G Grenade; V Knife; Esc Pause/Back; E свободна.
- Interact-held+Fire-press — отдельный бинд Grenade; Interact-held+mouse-motion — отдельный бинд Knife. Их можно удалить/очистить; они следуют переназначенным действиям.
- Удержание V повторяет горизонтальные взмахи слева направо90° с обычным интервалом игры. Для движения мышью сохраняется направленный взмах.
- Каждое действие допускает несколько биндов, удаление отдельного и очистку полного списка; held объединяется по всем источникам.
- Никакой эмуляции WiiMote, искусственного ускорения, hardware model, JIT или runtime-инструкций. Непроверенные семантические функции не заменяются предположениями.
- SDL-поток не пишет игровую память; межпоточные изменяемые данные синхронизированы. Фокус/смена конфигурации отменяют старые удержания и ожидающие команды.
- ALT освобождает курсор только в Windowed. Ctrl+E сохраняет существующий итоговый screenshot. После внедрения Pause, Esc больше не служит скрытой командой освобождения курсора.
- Диагностика видимая, со звуком, --skip-intro и отдельной копией сохранений; управление только внутри собственного процесса. ОС-ввод и захват пользовательского фокуса запрещены.
- Сборка в dist; установка в S:\Games\Umbrella Chronicles через robocopy /E. Не удалять файлы/планы/сохранения; коммиты только по запросу. Ghidra отложена.
- Этот план — этап1. Сохранение input в config.json, вкладка лаунчера и замена промптов остаются обязательными этапами2/3, не объявляются реализованными после этапа1.

## Review Focus

1. Несколько удерживаемых биндов и потеря фокуса: отпускание одного источника не отпускает другой, фокус отменяет всё — Task1/3.
2. Interact/Fire в разном порядке в одном обновлении: граната возникает только из согласованного сочетания, без двойного выстрела/расхода — Task1/4.
3. Очистка обычного и специального бинда: пустое назначение действительно отключено, исходная комбинация игры не активируется тайно — Task1/4.
4. Изменение игрового контекста при удержании: меню/QTE не получают случайные выстрелы или отложенные гранаты — Task2/3/4.
5. Высокая частота mouse-motion, несколько wheel-steps и длинный кадр: порядок кнопок сохраняется, очередь ограничена, нож соблюдает штатный cooldown — Task1/3/4.

---

## Границы файлов и интерфейсов

- `common/input_bindings.h/.cpp`: чистая модель Action/Binding/InputContext, defaults, валидация, резолвер. Библиотека `uc_input`, без SDL/игровой памяти.
- `runtime/native_input.h/.cpp`: существующее наведение/KPAD сохраняется; синхронизированная публикация host-событий и получение ActionFrame добавляются здесь.
- `game/host_input.h/.cpp`: преобразование SDL-events в типизированные физические события, служебные действия окна/screenshot.
- `game/game_input.h/.cpp`: проверенные RBUE08 точки контекста и семантических действий; отдельная `uc_game_input`, связанная с generated game.
- `project/hle_map.txt`, `project/uc.cfg`, `project/symbols.txt`: только подтверждённые HLE/continuation и их доказательства.
- `game/test_control.h`, `scripts/train_route.py`: совместимый процессный диагностический протокол, без ОС-ввода.
- Тесты `test_input_bindings`, `test_host_input`, `test_game_input`, существующие `test_input`/`test_options` и реальный видимый прогон.

Стабильные имена действий: `interact`, `fire`, `weapon_previous`, `weapon_next`, `reload`, `grenade`, `knife`, `pause`, `confirm`, `back`, `release_cursor`, `screenshot`.

## Task1: Общие бинды и упорядоченный резолвер

**Files:** Create `common/input_bindings.h`, `common/input_bindings.cpp`, `tests/test_input_bindings.cpp`; Modify `common/CMakeLists.txt`, `tests/CMakeLists.txt`.

**Interfaces:**
- `enum class Action:uint8_t` — перечисленные12 действий и count.
- `enum class InputContext:uint8_t { inactive,menu,gameplay,qte }`.
- `enum class PhysicalKind:uint8_t { key,mouse,wheel }`; `PhysicalInput { PhysicalKind kind; uint16_t code; }`. Keyboard code — SDL scancode1..511; mouse1..255; wheel1=up,2=down.
- `enum class BindingKind:uint8_t { physical,interact_fire,interact_motion }`; `Binding { BindingKind kind; PhysicalInput input; std::vector<PhysicalInput> modifiers; }`. Специальные виды допустимы только для Grenade/Knife соответственно, без physical/modifiers.
- `using Bindings=std::array<std::vector<Binding>,size_t(Action::count)>`; максимум64 назначений на действие,4 различных key/mouse modifiers. Wheel не является удерживаемым модификатором.
- `Bindings default_bindings(); std::optional<std::string> validate_bindings(const Bindings&);` Дубли внутри одного действия — ошибка; совпадения между действиями сохраняются.
- `ActionEvent { Action action; bool down; float direction_x,direction_y; }`; `ActionFrame { uint64_t held,pressed,released; std::array<ActionEvent,256> events; size_t count; }`.
- `class BindingResolver`: `replace(const Bindings&)`, `context(InputContext)`, `focus(bool)`, `button(PhysicalInput,bool)`, `wheel(int)`, `motion(float,float)`, `ActionFrame consume()`. Однопоточный чистый объект; пользовательские события передаются ему под mutex на следующем этапе.

- [ ] RED-тест defaults: все назначения таблицы спеки, свободная E, отдельные специальный гранатный/ножевой бинды.
- [ ] RED-тесты `multiple_sources`, `short_click`, `ordered_chord`, `clear_special`, `replace_held`, `focus_cancel`: два физических источника held, down/up до consume, Fire→Interact и Interact→Fire, G+chord один Grenade, отсутствие Fire на гранатном событии, очистка специальных сочетаний, новая конфигурация без случайного нажатия, фокус отменяет queued actions.
- [ ] RED-тесты валидации: неизвестные action/kind/code, wheel-модификатор, повтор modifiers, >64 записей и >4 modifiers; между контекстами один physical input допустим.
- [ ] Запустить `cmake --build build --target test_input_bindings` после `scripts\env.bat`; RED до реализации.
- [ ] Реализовать минимальный резолвер. Контексты фильтруют gameplay/menu actions; служебные actions доступны в активном окне. Wheel-steps сохраняют последовательность Previous/Next; OS key-repeat не создаёт дополнительных one-shot действий. Удерживаемый Knife — запрос повторения, частоту определяет игровой обработчик, а не резолвер.
- [ ] Ограничить очередь256 событиями; overflow явно сбрасывает удержания/очередь и сообщает ошибку, без частичного исполнения. Слить только последовательные motion-события до границы кнопки, сохранив реальное смещение; покрыть burst/cancellation тестом.
- [ ] GREEN `ctest --test-dir build -R test_input_bindings --output-on-failure`. Проверить отсутствие SDL/guest-memory зависимостей `uc_input`.

## Task2: Подтвердить семантические границы и контекст RBUE08

**Files:** Create `docs/spikes/I1-native-action-boundaries.md`, `game/input_probe.h`, `game/input_probe.cpp`; Modify `project/symbols.txt`, `runtime/native_input.h/.cpp`, `game/options.h/.cpp`, `game/uc_main.cpp`, `game/CMakeLists.txt`, `tests/test_input.cpp`, `tests/test_options.cpp`. Production HLE mappings пока не менять.

**Consumes:** согласованная семантика из спеки; кандидаты I0; `build/tools/xref/xref.exe`; исходный DOL и действующий процессный контроль.

**Produces:** для каждого действия таблица «entry/callsite, object, параметры, допустимые состояния, cooldown/animation, доказательство»; подтверждённый источник InputContext; реальные direction/arc поля Knife. Это данные для Task4, без придуманных адресов.

**Probe interface:** `using InputProbe=void(*)(const ppc::Ctx&,const InputSample&); void input_hle_set_probe(InputProbe);` — callback existing KPADRead перед возвратом, зарегистрирован до запуска guest и очищен после join. `void input_probe_init(bool); void input_probe_sample(const ppc::Ctx&,const InputSample&);` в game/input_probe; новый CLI `--input-probe` off по умолчанию. Только validated reads, максимум128 записей изменений состояния/назначений; после лимита остаются счётчики. Probe не меняет guest registers/memory или input edges.

- [ ] Разобрать исходный binding getter8003FB98/setter8003FB74 и живые назначения8045A808, а также места Interact/Fire/Weapon/Pause, используя xref и снимок уже инициализированного состояния под baton. Сырые статические данные таблицы не считать живым назначением.
- [ ] RED/GREEN probe fixture: callback видит фактический sample и context без изменения возвращаемого KPAD состояния; неверный buffer не вызывает probe и не теряет edges. Опция off оставляет прежнее поведение. Реализовать readonly snapshot только для доказанных mapped spans; ещё не найденные object addresses не разыменовывать.
- [ ] Проверить кандидат Reload8006D980 и его caller8006C268: начальное состояние, количество в магазине/резерве, штатная анимация, полный/пустой магазин, запрет повторного запуска во время Reload.
- [ ] Проверить Grenade и Knife как отдельные потребители. Общий motion detector80171D38 имеет несколько разных callers; глобальная подмена true запрещена. Установить точную границу удержания Knife и код выбора направления/90° дуги.
- [ ] Подтвердить контекст menu/gameplay/qte/paused/loading из реального объекта/состояния. Воспроизвести переходы меню→загрузка→бой→пауза→меню с внутренним вводом; ни FPS, ни таймер маршрута не определяют контекст.
- [ ] Снять bounded логи до/после действий: адрес объекта, состояние, ammo/grenades, animation id/cooldown и факт принятия запроса. Профиль выключен по умолчанию, не печатает каждый кадр и не записывает целиком ресурсы игры.
- [ ] Записать I1 и проверенные symbols с доказательствами. Выходной критерий: реальные вызовы всех выбранных действий и их ограничения подтверждены; отсутствующая граница остаётся явным блокером соответствующей части Task4.
- [ ] Если анализ требует Ghidra или действий владельца, остановить только зависимую часть, объяснить конкретную недостающую информацию и запросить её; сохранить уже выполненное. Никаких guessed HLE, fake shake или автоматической установки.

## Task3: SDL и межпоточная публикация

**Files:** Create `game/host_input.h`, `game/host_input.cpp`, `tests/test_host_input.cpp`; Modify `runtime/native_input.h/.cpp`, `game/uc_main.cpp`, `game/CMakeLists.txt`, `runtime/CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/test_input.cpp`.

**Consumes:** `uc_input` из Task1.

**Produces:** `NativeInput::bindings(const Bindings&)`, `NativeInput::context(InputContext)`, `NativeInput::host_button(PhysicalInput,bool)`, `NativeInput::host_wheel(int)`, `NativeInput::host_motion(float,float)`, `ActionFrame NativeInput::consume_actions()`, `ActionFrame NativeInput::consume_service_actions()`; `HostInput::event(const SDL_Event&,NativeInput&)` — адаптер существующего event-loop, с данными размера/режима окна переданными явно. Game/service результаты имеют отдельные очереди: потребление screenshot/release_cursor не забирает Fire/Reload и наоборот.

- [ ] RED: сгенерированные SDL-events проверяют ЛКМ Interact, ПКМ Fire, W/S, оба wheel направления, отсутствие E, scancode вместо layout-dependent keycode, key-repeat и focus loss. Проверка два бинда→отпускание одного сохраняет action held.
- [ ] RED: action publication/consume проверяют ordered snapshots под mutex, reset после смены биндов/контекста, отсутствие stale input. Существующие KPAD/probe/raw-aim тесты остаются зелёными.
- [ ] Реализовать host adapter и NativeInput API без игровых вызовов/записей памяти из SDL-потока. Наведение сохраняет существующее raw движение; host_motion для Knife использует тот же реальный поток событий без акселерометрических полей.
- [ ] Служебные release_cursor/screenshot читать через consume_service_actions в SDL owner loop отдельно от game consumption. RED/GREEN тест подтверждает, что screenshot+Fire в одном обновлении доставляются обоим владельцам по одному разу. Alt разрешён только Windowed; Ctrl+E сохраняет итоговый кадр. Смена фокуса/первый клик после release сохраняют нынешнее безопасное recapture без случайного игрового действия.
- [ ] Убрать прежние жёстко заданные button/key ветки после подключения адаптера; события не обрабатываются дважды. Esc становится Pause/Back, а захват снимается отдельным action.
- [ ] GREEN `test_host_input`, `test_input`, `test_window_mode`; короткий Windowed smoke с отдельными saves и без захвата фокуса.

## Task4: Нативные игровые действия и исходные сочетания

**Files:** Create `game/game_input.h`, `game/game_input.cpp`, `tests/test_game_input.cpp`; Modify `game/CMakeLists.txt`, `project/hle_map.txt`, `project/uc.cfg`, `game/uc_main.cpp`, `runtime/native_input.cpp`; Update `docs/spikes/I1-native-action-boundaries.md`, `docs/tech-debt.md`.

**Consumes:** доказательства Task2, ActionFrame Task1/3, существующие dispatch/memory-validation/scheduler safe points.

**Produces:** `void game_input_init(NativeInput*)`, `void game_input_shutdown()`, `void game_input_update(ppc::Ctx&)`. Game-level HLE entry names/continuations соответствуют доказанной таблице I1; изменения generated кода делаются через cfg/translator.

- [ ] RED fixtures для подтверждённых объектов/полей: Interact не Fire; выстрел расходует ammo/сохраняет частоту; Reload только из допустимого состояния; Grenade один расход на нажатие; полный/пустой reserve; unavailable action без мутации состояния.
- [ ] RED порядок Interact/Fire и очистка специальных биндов проверяются на игровой границе: отключённая combo не вызывает исходную скрытую гранату через A+B. При активном combo Fire подавлен; G+combo один бросок.
- [ ] RED Knife: V held повторяет только после штатного cooldown; mouse gesture при Interact задаёт направление; motion без Interact не бьёт; V+gesture один удар с направлением gesture; отмена при фокусе/контексте. Проверить90° через подтверждённое игровое представление.
- [ ] Реализовать адаптер под baton: потребление host actions ровно один раз, обновление проверенного InputContext, сохранение оригинальных game checks/animations/cooldowns. Native calls/compiled continuations с проверкой guest spans и ABI. Никаких global-motion true, искусственных IMU полей или blind вызовов из недопустимого состояния.
- [ ] Обычные KPAD поля сохранить для ещё используемых проверенных потребителей; исключить duplicate semantic actions. Границы, которые всё ещё читают neutral motion, остаются TD-041; не объявлять долг закрытым целиком.
- [ ] Регенерировать через существующий translate/build, GREEN `test_game_input` плюс связанные input/intro/pacing tests. Проверить QTE потребители, выявленные Task2: реальные окна/ошибки ввода, отсутствие автоматического успеха.

## Task5: Автономный игровой маршрут и замер

**Files:** Modify `game/test_control.h`, `scripts/train_route.py`, `tests/test_input.cpp`; Create `docs/spikes/I2-native-pc-controls-acceptance.md`; Modify this plan checklist and `docs/tech-debt.md`.

**Interfaces:** сохранить существующий протокол `sequence x y held capture`. Добавить строку `v2 sequence kind code value x y capture`: kind=`key|mouse|wheel|motion|focus`; key/mouse value0/1, wheel code1/2 и положительное число steps, motion x/y реальные finite deltas, focus value0/1. Служебные неиспользуемые поля обязаны0. Всего<=256bytes, sequence>0, capture0/1; acknowledgement после принятия/потребления. Старые raw A/B команды не подменяют новый binding test.

- [ ] RED malformed protocol, unknown kind/code, отрицательные/неfinite поля, stale sequence, overflow и одинаковая sequence; старый маршрут остаётся совместимым.
- [ ] Добавить v2 публикацию/ack через общий host-input путь. Только собственное diagnostic окно, isolated saves и Windowed, без ОС-событий/focus activation.
- [ ] Обновить маршрут: меню ЛКМ; в бою ПКМ, колесо/W/S, R, G/Interact+Fire, V/направленный жест. Пройти дальше первого вагона; снять кадры действий и проверить ammo/grenades/animation/state из I1. Геймплейный FPS исключает tutorial/death screens и PNG readback.
- [ ] Проверить live очищенные/переназначенные бинды через тот же модельный API. Сохранение config и UI ещё не готовы: это не заменяет обязательный этап2.
- [ ] Полный `build.bat`, CTest, актуальные Vulkan smoke fixtures при графических изменениях; установка dist в S через /E. Записать actual results и все оставшиеся ограничения в I2, без обещания не проверенных миссий/регионов.
- [ ] После выполнения всех задач плана — один свежий итоговый review на наиболее сильной доступной модели; исправить существенные находки, повторить только затронутые проверки. Коммит только по отдельной просьбе владельца.

## Обязательные последующие планы

**Этап2 — config и лаунчер.** Общий сериализованный input schema в config.json с отсутствующим разделом/defaults и явными пустыми списками; атомарная запись без потери прочих настроек. Dear ImGui/SDL3 вкладка «Управление», добавление нескольких назначений, удаление записи, «Очистить все», restore defaults, конфликты, отмена capture и явный выбор Esc. Версии JSON/ImGui и конкретные файлы фиксируются в этом отдельном плане после проверки зависимостей; реальные game adapter semantics не меняются.

**Этап3 — native prompts.** Проверенные UI-панели/menu/tutorial/QTE переводятся в semantic prompt requests. Host-owned иконки mouse/keyboard/chords/knife показывают действующие бинды, альтернативы и «Не назначено»; удаление исходного icon draw адресное. Общие игровые атласы и файлы game/DATA не меняются. UI следует разрешению вывода; capture содержит ровно итоговый UI. Непокрытые prompts фиксируются явно. План включает real menu/tutorial/QTE и GPU alpha/resize/fullscreen/readback проверки.

## Самопроверка плана

- Спека§1–3: Tasks1–5; multiple/clear/ordered combo/focus/native motion covered.
- Спека§4: общая модель Task1 и отдельный обязательный этап2; сохранение/launcher не выданы за часть готового этапа1.
- Спека§5: обязательный этап3; no global atlas replacement и no game-image writes.
- Спека§6: автономная visible/audible проверка Task5; Native, no commits/deletions preserved.
- Неподтверждённые адреса/состояния — явный исследовательский gate Task2, затем доказательства в I1. План не предполагает успешность Reload candidate.

Статус: согласован владельцем2026-10-13 («Выполняй. Перед выполнением сделай коммит»). Способ Native уже выбран; повторно не спрашивать способ исполнения. Начать задачи после checkpoint-коммита.
