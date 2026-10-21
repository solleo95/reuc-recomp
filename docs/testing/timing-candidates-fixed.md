# Исправленные кандидаты зависимости от FPS

## Продолжение — 2026-10-21, вечер

Четыре открытых кандидата закрыты: 8017AC88/8017AF70 исправлены (SoundWaitStep), 8012903C и 803ADC7C — не кадровые. Полный build: **135/135**, генерация `generation_86e9249dfeaf5139`. Пересканирование: 3 564 store PC, новых вне реестра нет. Установка в папку игры этим проходом не выполнялась. Подробности — [timing-handoff.md](timing-handoff.md).

## Обновление сборки и установки — 2026-10-21 16:37:21 МСК

Актуальные исходники, включая SoundRetryStep, прошли полный build: **135/135 тестов**, 14.58 с. Генерация: `generation_bb9b4e9eb8c69408`. Сборка установлена в `S:/Games/Umbrella Chronicles`; все **102** файла install manifest совпали с dist по SHA-256. [Состав и контрольные суммы](timing-latest-deployment.json). Игра после установки не запускалась.

Это обновление заменяет прежние замечания ниже о невыполненной полной сборке и отсутствии установки. Классификация кандидатов не менялась: четыре места остаются открытыми. Повторное сканирование новой генерации ещё не выполнялось.


Снимок работы от 2026-10-21. Адреса — guest PPC, шестнадцатеричные. Одна запись — инструкция записи в память (store PC), а не отдельный баг. Несколько записей могут исправляться одним hook. Технические обоснования сохранены на языке исходных исследовательских записей, чтобы не потерять смысл и адреса.

`fixed` означает наличие исправления в исходниках; установка и проверка конкретной сборки описаны отдельно. Символические регистры и смещения не являются доказанными типами объектов. Отсутствие прямого вызывающего адреса не исключает виртуальные вызовы.

[Состояние работы и продолжение](timing-handoff.md). Машиночитаемые записи содержат также прямые исходящие вызовы, происхождение сканирования и SHA-256 исходников.

**54 уникальных store PC**, включая ранее сделанные исправления. Дополнительный перечень ниже сохраняет исправления через вызовы функций, которые не представлены отдельной записью локального сканера.

**Сборка:** актуальная версия с SoundRetryStep прошла 135/135 тестов и установлена в папку игры; подробности в обновлении выше.

[Данные JSON](timing-candidates-fixed.json) · [Полный реестр](timing-candidates-all.md)

## Исправления, которые не сводятся к одному найденному store

Этот раздел дополняет 54 записи: локальный сканер не видит все изменения через вызовы функций. Ниже также сохранены ранние исправления, существенные для продолжения. Реализация — [game_pacing.cpp](../../game/game_pacing.cpp), объявления — [game_pacing.h](../../game/game_pacing.h), привязки — [hle_map.txt](../../project/hle_map.txt), retained AOT bodies — [uc.cfg](../../project/uc.cfg), регрессии — [test_game_pacing.cpp](../../tests/test_game_pacing.cpp).

| Механизм / адрес | Что делает и что исправлено | Проверка / предел |
| --- | --- | --- |
| VectorAddStep, 80064C34; вызов 801C3878, LR 801C387C | Падение убитой летучей мыши: добавление скорости к позиции получает масштаб номинального шага; гравитация уже использует dt. | Регрессии 30–480 Гц; полный полёт и столкновения требуют визуальной проверки. |
| JumpBlendStep, 80123D14; вызов 80200B88 | Прыжок обезьяны: повторная интерполяция текущей позиции к цели получает alpha = 1 − (1 − alpha)^scale. | Офлайн проверена композиция; меняющаяся alpha оставляет обычную погрешность интегрирования. Отдельный баг падения мёртвой обезьяны не подтверждён. |
| FlightStep 801C4438; FlightPathStep 801C44B8 | Скорость общего полёта и прохождение пути масштабируются только в ветвях без dt; режимы 16–20 уже учитывают время. | Регрессии по режимам; не доказана полная эквивалентность коллизий. |
| FlightPhaseStep 801C56F0 | Фазы 32/34 используют временно масштабированную скорость +8176; исходные биты восстанавливаются. | Реальное retained AOT тело: переход через 0.8 с при 30–480 Гц. |
| WorldRaycast 80044D14; LR 80200DFC | Исправление луча приземления обезьяны ограничено конкретным вызовом. | Более раннее исправление; новый полный прыжок в игре не проверялся. |
| ActorStepTime 800C790C; FixedPlaybackStep 800329FC, LR 800C799C | Остановка времени на паузе; шаг анимации обычного model node масштабируется в точном вызове. Animated-model context уже получает elapsed playback. | Не расширять масштабирование на getter 80032AA0 или вызов 80031344. |
| PhysicsStep 801172A4; PhysicsHit 80117464 | Шаг физики учитывает dt/паузу; импульс попадания сохраняет событийный смысл. | Более ранние исправления. |
| PropStep 80210278 | Пружинная физика предметов исполняется с номинальной частотой 60 Гц; новый объект на паузе не получает первый лишний шаг. | Регрессии паузы и шага; 60 Гц здесь намеренно. |
| PhysicsAutoDisable 800F1D08; уничтожение тела 800EA9B4 | Quiet-step счётчик учитывает дробный шаг каждого тела; движение сбрасывает остаток, уничтожение удаляет его. Секундный таймер уже использует dt. | Офлайн сравнение исходного AOT: все 352 байта тела совпали в 9 600 случаях номинального шага. |
| CameraHistoryStep 801B0AAC | Кольцо истории камеры получает номинальные выборки и интерполяцию между ними; сохраняется задержка. | Управляемое тело-заглушка, линейные траектории, пауза, переменный шаг. Полная сцена не проверена. |
| FrameWait: 806C6158, 806C6444, 806C6B50, 806C8A18, 806C4EA8 | Масштабируются уникальные коэффициенты затухания света, UV, текстурной прокрутки, истории модели и slow motion. | Подтверждены отдельные читатели коэффициентов; экспоненциальное затухание использует степень scale. |
| FrameWait: 806C6B90/6B8C и 806C6B98/6B94 | Две десятислойные текстурные скорости: масштабируются и наклон, и свободный член. | Регрессии коэффициентов; визуальный результат воды отдельно не подтверждён этим аудитом. |
| ModelHistoryIdentity 80264680; LR 801929B0 | Исправлена отмена dt в (10*dt)/(20*dt): числитель получает масштаб номинального шага. | Ограничено одним вызовом, обычная identity-матрица сохраняется. |
| DeferredActorStep 801A5130 | Задержка действия +8488 считает номинальные тики, обычное обновление актёра остаётся на каждом вызове. | Регрессии частот и паузы. |
| RumbleStep 80136EEC | Байты шаблона вибрации расходуются по номинальным тикам. | Проверено офлайн; физический Wii Remote не проверен. |
| TrailHistoryStep 80162A68; WeaponTrailStep 8006DB34 | История следа и восемь образцов оружия обновляются по номинальным тикам; проекция и dt-затухание продолжаются правильно. | При догоняющих тиках повторяется текущий образец; форма при низком FPS требует проверки. |
| UiPhysicsStep 802447D8; gravity 806C9CF4 | Гравитация UI и угловое торможение +108 масштабируются; настройка торможения восстанавливается, включая исключение. | Регрессии временного изменения параметров и шага. |
| TimelineStep 80211734 | Сценарные события сравниваются со временем в номинальных кадрах; значение clock +42312 растёт на scale. | Проверен собственный пролог и управляемое retained тело; реальный сценарий целиком не проигрывался. |
| EffectRetireStep 801B916C | Задержка удаления эффекта учитывает номинальные тики; поиск объектов и проверка окончания анимации продолжаются каждый вызов. | Изменяются только сохранившиеся счётчики тех же записей; регрессии замены и удаления. |
| TargetRefreshStep 8012AEE8; LR 801297B0 | 90-кадровый счётчик обновления списка целей 801297BC учитывает номинальные тики. | Другой глобальный счётчик 8012903C — кэш сцены, не кадровый. |
| HudNameDelay 8014D368 | Ожидание готовности имени HUD, поле +7752, считает номинальные кадры. | Ранее существовавшее исправление включено в реестр. |
| SoundFrameStep 803AA7B0 | Вся защищённая блокировкой обработка списка голосов идёт по номинальным тикам, сохраняя удаление и сортировку; AX остаётся на таймере 3 мс. | Проверены частота и ABI wrapper; реальный звук не прослушивался. |
| SoundRetryStep 8004C91C; LR 8004A6FC | Шесть попыток запуска звука больше не истекают за шесть render updates; компенсируется уже выполненный decrement 8004A6D8. Начальные запросы не меняются. | Focused tests и полный build 135/135 прошли; правка включена в dist и установленную сборку. При catch-up одна попытка на вызов; проверить, если готовность меняется внутри пропущенного интервала. |
| HomeMenuStep 803C1BF8 | Таймеры, затухание и дочерние обновления меню идут по номинальным тикам; press/release события накапливаются и потребляются однократно. | Проверены короткий клик, пауза, init, смена контроллера, catch-up, завершение и восстановление KPAD. Повторные клики внутри одного тика объединяются. |
| CrossfadeStep 80165610; LR 8016975C | Кроссфейд слоёв анимации: elapsed умножался на playback, который игра уже ставит в кадрах за вызов → переходы в 16 раз медленнее при 480 Гц; у обезьян почти пропадал root motion, ИИ не доходил до игрока. Elapsed делится на step_scale. | Регрессии 30–480 Гц, пауза, чужой LR; A/B обезьян 30/480 Гц совпал по дистанции и скоростям; train_gameplay PASS. Остальные актёры визуально не проверены. |
| SoundWaitStep 80049000; LR 8017AC74/8017AF5C | Ожидание готовности звукового потока в состояниях 698/834 (8017A7B0) длится 30 номинальных кадров (1 с), а не 30 обновлений (62 мс при 480 Гц). Другие вызовы 80049000 без изменений. | Регрессии 30–480 Гц, пауза, готовый поток, чужие LR; полный build 135/135. Живой звук не проверялся. |
| MovieUpdate 801426D8 | Ранее существовавший hook доводит пропуск видео до конца fade и очистки декодера. | Включён как покрытие кандидатов, не как новый фикс этой итерации. |

## Исправленные store PC

Далее все 54 операции со своими описаниями и доказательствами.

## 8004A6D8 — функция 8004A3CC

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Six-frame sound retry window previously expired after six render updates. SoundRetryStep compensates the caller decrement atLR8004A6FC, preserves pending retries on fractional/paused frames, and consumes nominal elapsed ticks before a retry. Initial sound requests retain their original behavior.

**Происхождение:** manual-call-chain.

**Доказательства / связанные адреса:**

- 8003A680
- 8004A6C8-8004A704
- 8004CAFC-8004CB0C
- game/game_pacing.cpp:hle_SoundRetryStep
- tests/test_game_pacing.cpp
## 800667C0 — функция 8006648C

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Clock channel-1 slowdown ramp subtracts 0.12 each update; scale sole-reader literal by nominal elapsed frames.

**Происхождение:** baseline-3513.

**Поле:** `r31@block81+0x128`. Тип: `float-add`; операции: `fsubs`. Приоритет сканера: `high`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_006.cpp:6442`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_006.cpp:6219; investigator source review; 800667C0/820/890 clock reads; field/cadence unresolved
- 800667B0/B8/C0 decrement actor+296; 8006686C/8DC pass it to clock channel 1 via 80158270; 801582C8 multiplies raw delta by channels. Literal 806C4EA8 read only at 800667B0. Regression equal ramp at 30–480 Hz, pause and nominal restoration.
## 8006E140 — функция 8006DB34

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Eight-sample weapon trail advanced each update; native wrapper keeps fade elapsed-time based and samples original geometry at nominal cadence.

**Происхождение:** baseline-3513.

**Поле:** `r30@block58+0x49C`. Тип: `integer-step`; операции: `addi`. Приоритет сканера: `medium`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_007.cpp:7152`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_007.cpp:6799; investigator source review; clock reads; three field stores; timing semantics unresolved
- 8006DB94..F4 fade dt;8006DC00..60 samples pointer;8006E134..174 ringadvance;WeaponTrailStep entry8006DBF8 tests30..480/pause/catchup
## 800F1D8C — функция 800F1D08

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Quiet-step counter normalized using game_step_scale with fractional carry; seconds timer already consumes solver dt.

**Происхождение:** baseline-3513.

**Поле:** `r4@block9+0x15C`. Тип: `integer-step`; операции: `addi`. Приоритет сканера: `inspect-loop`.

**Исходная привязка:** `generated/generation_8f5c8a9f29ed8c00/game_020.cpp:7469`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- 12 captured bodies: configured +336=0, +340=10; original sleep after11calls
- game/game_pacing.cpp hle_PhysicsAutoDisable; tests/test_game_pacing.cpp 30..480Hz and lifetime/reset cases
- build/sleep_probe.exe:9600 comparisons of body bytes with original AOT at nominal rate

**Прямые вызывающие функции:** `800F215C`.
## 801297BC — функция 80129004

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** 90-call target refresh countdown now advances with nominal30Hz ticks; original state helper remains every update.

**Происхождение:** baseline-3513.

**Поле:** `r30@block171+0x1C`. Тип: `integer-step`; операции: `addi`. Приоритет сканера: `medium`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_028.cpp:1246`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_028.cpp:1336 /*801297B4*/ { ppc::op_addi(c, 3, 30, 56); } | /*801297B8*/ { ppc::op_addi(c, 0, 4, -1); } | /*801297BC*/ { ppc::op_stw(c, 0, 30, 28); } | /*801297C0*/ { c.lr = 0x801297C4u; fn_8003F1D4(c); }
- 80129968..70 reset90;801297B0..BC decrement;TargetRefreshStep after8012AEE8 adjusts next subtraction;30..480FPS andpause tests
## 80136F3C — функция 80136EEC

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Rumble pattern index advanced once per render/update call; RumbleStep now executes original pattern body once per nominal 30 Hz tick.

**Происхождение:** baseline-3513.

**Поле:** `r30@block6+0xC`. Тип: `integer-step`; операции: `addi`. Приоритет сканера: `inspect-loop`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_029.cpp:4891`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_029.cpp:4937 /*80136F34*/ { ppc::op_lwz(c, 3, 30, 12); } | /*80136F38*/ { ppc::op_addi(c, 4, 3, 1); } | /*80136F3C*/ { ppc::op_stw(c, 4, 30, 12); } | /*80136F40*/ { ppc::op_lwz(c, 0, 30, 8); } callers=80250A44
- 80136F34..80136F94 index/sample/end;80137020/60 motor transitions;caller80250A64;30..480 Hz/pause/catchup regression
## 80140C84 — функция 8014061C

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Lighting residual retention now pow(.92, nominal step) at frame boundary.

**Происхождение:** baseline-3513.

**Поле:** `r26@block92+0x1C`. Тип: `float-add`; операции: `fsubs`, `fmadds`. Приоритет сканера: `high`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_030.cpp:7472`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_030.cpp:7315 /*801409AC*/ { ppc::op_psq_l(c, 0, 29, 0, 0, 0); } | /*801409B0*/ { ppc::op_ps_muls0(c, 0, 0, 1, false); } | /*801409B4*/ { ppc::op_psq_st(c, 0, 29, 0, 0, 0); } | /*801409B8*/ { ppc::op_psq_l(c, 0, 29, 8, 1, 0); } callers=80142230,801423E0,801B2050
- game_030.cpp:7318 /*801409B8*/ { ppc::op_psq_l(c, 0, 29, 8, 1, 0); } | /*801409BC*/ { ppc::op_ps_muls0(c, 0, 0, 1, false); } | /*801409C0*/ { ppc::op_psq_st(c, 0, 29, 8, 1, 0); } | /*801409C4*/ { /* cr0 compare is folded into the next branch */ } callers=80142230,801423E0,801B2050
- game_030.cpp:7437 /*80140B50*/ { ppc::op_lfs(c, 2, 3, 0); } | /*80140B54*/ { ppc::op_fmuls(c, 6, 2, 5, false); } | /*80140B58*/ { ppc::op_stfs(c, 6, 3, 0); } | /*80140B5C*/ { ppc::op_fcmpo(c, 0, 6, 1); } callers=80142230,801423E0,801B2050
- game_030.cpp:7441 /*80140B60*/ { ppc::op_lfs(c, 2, 3, 4); } | /*80140B64*/ { ppc::op_fmuls(c, 2, 2, 4, false); } | /*80140B68*/ { ppc::op_stfs(c, 2, 3, 4); } | /*80140B6C*/ { ppc::op_lfs(c, 2, 3, 8); } callers=80142230,801423E0,801B2050
- game_030.cpp:7444 /*80140B6C*/ { ppc::op_lfs(c, 2, 3, 8); } | /*80140B70*/ { ppc::op_fmuls(c, 2, 2, 3, false); } | /*80140B74*/ { ppc::op_stfs(c, 2, 3, 8); } | /*80140B78*/ { { if (ppc::crbit(c, 1) == false) { goto L_80140B84; } } } callers=80142230,801423E0,801B2050
- game_030.cpp:7519 /*80140C7C*/ { ppc::op_fmadds(c, 0, 1, 2, 0, false); } | /*80140C80*/ { ppc::op_stfs(c, 0, 1, 280); } | /*80140C84*/ { ppc::op_stfs(c, 4, 26, 28); } | /*80140C88*/ { ppc::op_lfs(c, 2, 26, 40); } callers=80142230,801423E0,801B2050
- game_030.cpp:7554 /*80140D08*/ { ppc::op_fmadds(c, 0, 1, 4, 0, false); } | /*80140D0C*/ { ppc::op_stfs(c, 0, 1, 308); } | /*80140D10*/ { ppc::op_stfs(c, 7, 26, 40); } | /*80140D14*/ { ppc::op_lfs(c, 0, 1, 292); } callers=80142230,801423E0,801B2050
- 80140C4C sole reader of806C6158; original old-target blend for9channels; test_game_pacing 30..480/pause/mode restore.

**Прямые вызывающие функции:** `80142230`, `801423E0`, `801B2050`.
## 80140D10 — функция 8014061C

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Lighting residual retention now pow(.92, nominal step) at frame boundary.

**Происхождение:** baseline-3513.

**Поле:** `r26@block92+0x28`. Тип: `float-add`; операции: `fsubs`, `fmadds`. Приоритет сканера: `high`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_030.cpp:7507`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_030.cpp:7315 /*801409AC*/ { ppc::op_psq_l(c, 0, 29, 0, 0, 0); } | /*801409B0*/ { ppc::op_ps_muls0(c, 0, 0, 1, false); } | /*801409B4*/ { ppc::op_psq_st(c, 0, 29, 0, 0, 0); } | /*801409B8*/ { ppc::op_psq_l(c, 0, 29, 8, 1, 0); } callers=80142230,801423E0,801B2050
- game_030.cpp:7318 /*801409B8*/ { ppc::op_psq_l(c, 0, 29, 8, 1, 0); } | /*801409BC*/ { ppc::op_ps_muls0(c, 0, 0, 1, false); } | /*801409C0*/ { ppc::op_psq_st(c, 0, 29, 8, 1, 0); } | /*801409C4*/ { /* cr0 compare is folded into the next branch */ } callers=80142230,801423E0,801B2050
- game_030.cpp:7437 /*80140B50*/ { ppc::op_lfs(c, 2, 3, 0); } | /*80140B54*/ { ppc::op_fmuls(c, 6, 2, 5, false); } | /*80140B58*/ { ppc::op_stfs(c, 6, 3, 0); } | /*80140B5C*/ { ppc::op_fcmpo(c, 0, 6, 1); } callers=80142230,801423E0,801B2050
- game_030.cpp:7441 /*80140B60*/ { ppc::op_lfs(c, 2, 3, 4); } | /*80140B64*/ { ppc::op_fmuls(c, 2, 2, 4, false); } | /*80140B68*/ { ppc::op_stfs(c, 2, 3, 4); } | /*80140B6C*/ { ppc::op_lfs(c, 2, 3, 8); } callers=80142230,801423E0,801B2050
- game_030.cpp:7444 /*80140B6C*/ { ppc::op_lfs(c, 2, 3, 8); } | /*80140B70*/ { ppc::op_fmuls(c, 2, 2, 3, false); } | /*80140B74*/ { ppc::op_stfs(c, 2, 3, 8); } | /*80140B78*/ { { if (ppc::crbit(c, 1) == false) { goto L_80140B84; } } } callers=80142230,801423E0,801B2050
- game_030.cpp:7519 /*80140C7C*/ { ppc::op_fmadds(c, 0, 1, 2, 0, false); } | /*80140C80*/ { ppc::op_stfs(c, 0, 1, 280); } | /*80140C84*/ { ppc::op_stfs(c, 4, 26, 28); } | /*80140C88*/ { ppc::op_lfs(c, 2, 26, 40); } callers=80142230,801423E0,801B2050
- game_030.cpp:7554 /*80140D08*/ { ppc::op_fmadds(c, 0, 1, 4, 0, false); } | /*80140D0C*/ { ppc::op_stfs(c, 0, 1, 308); } | /*80140D10*/ { ppc::op_stfs(c, 7, 26, 40); } | /*80140D14*/ { ppc::op_lfs(c, 0, 1, 292); } callers=80142230,801423E0,801B2050
- 80140C4C sole reader of806C6158; original old-target blend for9channels; test_game_pacing 30..480/pause/mode restore.

**Прямые вызывающие функции:** `80142230`, `801423E0`, `801B2050`.
## 80142728 — функция 801426D8

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Existing MovieUpdate hook completes the skip fade in one transition, including decoder cleanup; candidate state+6 is enum 1→2, other fields forced to final fade step.

**Происхождение:** baseline-3513.

**Поле:** `r3@block8+0x8`. Тип: `float-add`; операции: `fadds`. Приоритет сканера: `review-time-use`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_030.cpp:9404`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- /*80142714*/ { ppc::op_lfs(c, 0, 3, 8); }
- /*80142720*/ { ppc::op_lfs(c, 1, 4, 4); }
- /*80142724*/ { ppc::op_fadds(c, 0, 0, 1, false); }
- /*80142728*/ { ppc::op_stfs(c, 0, 3, 8); }
- game/intro.cpp hle_MovieUpdate
- tests/test_intro.cpp movie skip and cleanup
## 8014281C — функция 801426D8

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Existing MovieUpdate hook completes the skip fade in one transition, including decoder cleanup; candidate state+6 is enum 1→2, other fields forced to final fade step.

**Происхождение:** baseline-3513.

**Поле:** `r31@block29+0x0`. Тип: `integer-step`; операции: `addi`. Приоритет сканера: `medium`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_030.cpp:9472`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_030.cpp:9451 /*80142720*/ { ppc::op_lfs(c, 1, 4, 4); } | /*80142724*/ { ppc::op_fadds(c, 0, 0, 1, false); } | /*80142728*/ { ppc::op_stfs(c, 0, 3, 8); } | /*8014272C*/ { ppc::op_addi(c, 3, 30, 13352); }
- game_030.cpp:9507 /*801427E8*/ { ppc::op_addi(c, 0, 4, 1); } | /*801427EC*/ { ppc::op_sth(c, 5, 31, 4); } | /*801427F0*/ { ppc::op_sth(c, 0, 31, 6); } | /*801427F4*/ { goto L_80142874; }
- game_030.cpp:9519 /*80142814*/ { ppc::op_addi(c, 0, 3, -10); } | /*80142818*/ { ppc::op_cmpi(c, 0, 0, -960); } | /*8014281C*/ { ppc::op_stw(c, 0, 31, 0); } | /*80142820*/ { { if (ppc::crbit(c, 0) == false) { goto L_8014282C; } } }
- game_030.cpp:9527 /*80142830*/ { ppc::op_addi(c, 3, 3, 8); } | /*80142834*/ { ppc::op_extsh(c, 3, 0, false); } | /*80142838*/ { ppc::op_sth(c, 3, 31, 4); } | /*8014283C*/ { ppc::op_cmpi(c, 0, 0, 255); }
- game/intro.cpp hle_MovieUpdate
- tests/test_intro.cpp movie skip and cleanup
## 80142838 — функция 801426D8

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Existing MovieUpdate hook completes the skip fade in one transition, including decoder cleanup; candidate state+6 is enum 1→2, other fields forced to final fade step.

**Происхождение:** baseline-3513.

**Поле:** `r31@block31+0x4`. Тип: `integer-step`; операции: `addi`. Приоритет сканера: `medium`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_030.cpp:9480`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_030.cpp:9451 /*80142720*/ { ppc::op_lfs(c, 1, 4, 4); } | /*80142724*/ { ppc::op_fadds(c, 0, 0, 1, false); } | /*80142728*/ { ppc::op_stfs(c, 0, 3, 8); } | /*8014272C*/ { ppc::op_addi(c, 3, 30, 13352); }
- game_030.cpp:9507 /*801427E8*/ { ppc::op_addi(c, 0, 4, 1); } | /*801427EC*/ { ppc::op_sth(c, 5, 31, 4); } | /*801427F0*/ { ppc::op_sth(c, 0, 31, 6); } | /*801427F4*/ { goto L_80142874; }
- game_030.cpp:9519 /*80142814*/ { ppc::op_addi(c, 0, 3, -10); } | /*80142818*/ { ppc::op_cmpi(c, 0, 0, -960); } | /*8014281C*/ { ppc::op_stw(c, 0, 31, 0); } | /*80142820*/ { { if (ppc::crbit(c, 0) == false) { goto L_8014282C; } } }
- game_030.cpp:9527 /*80142830*/ { ppc::op_addi(c, 3, 3, 8); } | /*80142834*/ { ppc::op_extsh(c, 3, 0, false); } | /*80142838*/ { ppc::op_sth(c, 3, 31, 4); } | /*8014283C*/ { ppc::op_cmpi(c, 0, 0, 255); }
- game/intro.cpp hle_MovieUpdate
- tests/test_intro.cpp movie skip and cleanup
## 8014D514 — функция 8014D36C

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** ExistingHudNameDelaynormalizesname-readinesscountertonominalframes.

**Происхождение:** baseline-3513.

**Поле:** `r30@block34+0x1E48`. Тип: `integer-step`; операции: `addi`. Приоритет сканера: `medium`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_032.cpp:3148`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_032.cpp:3164 /*8014D50C*/ { ppc::op_lwz(c, 3, 30, 7752); } | /*8014D510*/ { ppc::op_addi(c, 0, 3, 1); } | /*8014D514*/ { ppc::op_stw(c, 0, 30, 7752); } | L_8014D518:;
- hle_HudNameDelayobject+7752park11whennottickdue;8014D514coveredexactcounter.
## 80158CC4 — функция 80158860

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** UV phase speed .02 scales with nominal step; existing dt phase/wrap unchanged.

**Происхождение:** baseline-3513.

**Поле:** `r25@block47+0xA8`. Тип: `float-add`; операции: `fsubs`, `fmuls`, `fnmsubs`. Приоритет сканера: `inspect-loop`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_033.cpp:906`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_033.cpp:892 /*80158C60*/ { ppc::op_fnmsubs(c, 0, 2, 0, 1, false); } | /*80158C64*/ { ppc::op_fcmpo(c, 0, 0, 21); } | /*80158C68*/ { ppc::op_stfs(c, 0, 25, 200); } | /*80158C6C*/ { { if (ppc::crbit(c, 0) == false) { goto L_80158CA0; } } }
- game_033.cpp:896 /*80158C70*/ { ppc::op_lfs(c, 0, 25, 200); } | /*80158C74*/ { ppc::op_fadds(c, 0, 0, 22, false); } | /*80158C78*/ { ppc::op_stfs(c, 0, 25, 200); } | /*80158C7C*/ { goto L_80158CA0; }
- game_033.cpp:904 /*80158C8C*/ { ppc::op_fmadds(c, 0, 2, 0, 1, false); } | /*80158C90*/ { ppc::op_fcmpo(c, 0, 0, 22); } | /*80158C94*/ { ppc::op_stfs(c, 0, 25, 200); } | /*80158C98*/ { { if (ppc::crbit(c, 1) == false) { goto L_80158CA0; } } }
- game_033.cpp:917 /*80158CBC*/ { ppc::op_fnmsubs(c, 0, 1, 0, 25, false); } | /*80158CC0*/ { ppc::op_fcmpo(c, 0, 0, 21); } | /*80158CC4*/ { ppc::op_stfs(c, 0, 25, 168); } | /*80158CC8*/ { { if (ppc::crbit(c, 0) == false) { goto L_80158CD8; } } }
- game_033.cpp:921 /*80158CCC*/ { ppc::op_lfs(c, 0, 25, 168); } | /*80158CD0*/ { ppc::op_fadds(c, 0, 0, 26, false); } | /*80158CD4*/ { ppc::op_stfs(c, 0, 25, 168); } | L_80158CD8:;
- game_033.cpp:930 /*80158CEC*/ { ppc::op_fnmsubs(c, 0, 28, 0, 1, false); } | /*80158CF0*/ { ppc::op_fcmpo(c, 0, 0, 21); } | /*80158CF4*/ { ppc::op_stfs(c, 0, 25, 184); } | /*80158CF8*/ { { if (ppc::crbit(c, 0) == false) { goto L_80158D08; } } }
- game_033.cpp:934 /*80158CFC*/ { ppc::op_lfs(c, 0, 25, 184); } | /*80158D00*/ { ppc::op_fadds(c, 0, 0, 26, false); } | /*80158D04*/ { ppc::op_stfs(c, 0, 25, 184); } | L_80158D08:;
- 80158C08 sole reader of806C6444; f24 used only at58CB8/58CE8, position direction from projectedpoint stack40/44/48. test_game_pacing30..480/pause.
## 80158CD4 — функция 80158860

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** UV phase speed .02 scales with nominal step; existing dt phase/wrap unchanged.

**Происхождение:** baseline-3513.

**Поле:** `r25@block48+0xA8`. Тип: `float-add`; операции: `fadds`. Приоритет сканера: `inspect-loop`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_033.cpp:910`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_033.cpp:892 /*80158C60*/ { ppc::op_fnmsubs(c, 0, 2, 0, 1, false); } | /*80158C64*/ { ppc::op_fcmpo(c, 0, 0, 21); } | /*80158C68*/ { ppc::op_stfs(c, 0, 25, 200); } | /*80158C6C*/ { { if (ppc::crbit(c, 0) == false) { goto L_80158CA0; } } }
- game_033.cpp:896 /*80158C70*/ { ppc::op_lfs(c, 0, 25, 200); } | /*80158C74*/ { ppc::op_fadds(c, 0, 0, 22, false); } | /*80158C78*/ { ppc::op_stfs(c, 0, 25, 200); } | /*80158C7C*/ { goto L_80158CA0; }
- game_033.cpp:904 /*80158C8C*/ { ppc::op_fmadds(c, 0, 2, 0, 1, false); } | /*80158C90*/ { ppc::op_fcmpo(c, 0, 0, 22); } | /*80158C94*/ { ppc::op_stfs(c, 0, 25, 200); } | /*80158C98*/ { { if (ppc::crbit(c, 1) == false) { goto L_80158CA0; } } }
- game_033.cpp:917 /*80158CBC*/ { ppc::op_fnmsubs(c, 0, 1, 0, 25, false); } | /*80158CC0*/ { ppc::op_fcmpo(c, 0, 0, 21); } | /*80158CC4*/ { ppc::op_stfs(c, 0, 25, 168); } | /*80158CC8*/ { { if (ppc::crbit(c, 0) == false) { goto L_80158CD8; } } }
- game_033.cpp:921 /*80158CCC*/ { ppc::op_lfs(c, 0, 25, 168); } | /*80158CD0*/ { ppc::op_fadds(c, 0, 0, 26, false); } | /*80158CD4*/ { ppc::op_stfs(c, 0, 25, 168); } | L_80158CD8:;
- game_033.cpp:930 /*80158CEC*/ { ppc::op_fnmsubs(c, 0, 28, 0, 1, false); } | /*80158CF0*/ { ppc::op_fcmpo(c, 0, 0, 21); } | /*80158CF4*/ { ppc::op_stfs(c, 0, 25, 184); } | /*80158CF8*/ { { if (ppc::crbit(c, 0) == false) { goto L_80158D08; } } }
- game_033.cpp:934 /*80158CFC*/ { ppc::op_lfs(c, 0, 25, 184); } | /*80158D00*/ { ppc::op_fadds(c, 0, 0, 26, false); } | /*80158D04*/ { ppc::op_stfs(c, 0, 25, 184); } | L_80158D08:;
- 80158C08 sole reader of806C6444; f24 used only at58CB8/58CE8, position direction from projectedpoint stack40/44/48. test_game_pacing30..480/pause.
## 80158CF4 — функция 80158860

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** UV phase speed .02 scales with nominal step; existing dt phase/wrap unchanged.

**Происхождение:** baseline-3513.

**Поле:** `r25@block49+0xB8`. Тип: `float-add`; операции: `fsubs`, `fmuls`, `fnmsubs`. Приоритет сканера: `inspect-loop`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_033.cpp:919`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_033.cpp:892 /*80158C60*/ { ppc::op_fnmsubs(c, 0, 2, 0, 1, false); } | /*80158C64*/ { ppc::op_fcmpo(c, 0, 0, 21); } | /*80158C68*/ { ppc::op_stfs(c, 0, 25, 200); } | /*80158C6C*/ { { if (ppc::crbit(c, 0) == false) { goto L_80158CA0; } } }
- game_033.cpp:896 /*80158C70*/ { ppc::op_lfs(c, 0, 25, 200); } | /*80158C74*/ { ppc::op_fadds(c, 0, 0, 22, false); } | /*80158C78*/ { ppc::op_stfs(c, 0, 25, 200); } | /*80158C7C*/ { goto L_80158CA0; }
- game_033.cpp:904 /*80158C8C*/ { ppc::op_fmadds(c, 0, 2, 0, 1, false); } | /*80158C90*/ { ppc::op_fcmpo(c, 0, 0, 22); } | /*80158C94*/ { ppc::op_stfs(c, 0, 25, 200); } | /*80158C98*/ { { if (ppc::crbit(c, 1) == false) { goto L_80158CA0; } } }
- game_033.cpp:917 /*80158CBC*/ { ppc::op_fnmsubs(c, 0, 1, 0, 25, false); } | /*80158CC0*/ { ppc::op_fcmpo(c, 0, 0, 21); } | /*80158CC4*/ { ppc::op_stfs(c, 0, 25, 168); } | /*80158CC8*/ { { if (ppc::crbit(c, 0) == false) { goto L_80158CD8; } } }
- game_033.cpp:921 /*80158CCC*/ { ppc::op_lfs(c, 0, 25, 168); } | /*80158CD0*/ { ppc::op_fadds(c, 0, 0, 26, false); } | /*80158CD4*/ { ppc::op_stfs(c, 0, 25, 168); } | L_80158CD8:;
- game_033.cpp:930 /*80158CEC*/ { ppc::op_fnmsubs(c, 0, 28, 0, 1, false); } | /*80158CF0*/ { ppc::op_fcmpo(c, 0, 0, 21); } | /*80158CF4*/ { ppc::op_stfs(c, 0, 25, 184); } | /*80158CF8*/ { { if (ppc::crbit(c, 0) == false) { goto L_80158D08; } } }
- game_033.cpp:934 /*80158CFC*/ { ppc::op_lfs(c, 0, 25, 184); } | /*80158D00*/ { ppc::op_fadds(c, 0, 0, 26, false); } | /*80158D04*/ { ppc::op_stfs(c, 0, 25, 184); } | L_80158D08:;
- 80158C08 sole reader of806C6444; f24 used only at58CB8/58CE8, position direction from projectedpoint stack40/44/48. test_game_pacing30..480/pause.
## 80158D04 — функция 80158860

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** UV phase speed .02 scales with nominal step; existing dt phase/wrap unchanged.

**Происхождение:** baseline-3513.

**Поле:** `r25@block50+0xB8`. Тип: `float-add`; операции: `fadds`. Приоритет сканера: `inspect-loop`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_033.cpp:923`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_033.cpp:892 /*80158C60*/ { ppc::op_fnmsubs(c, 0, 2, 0, 1, false); } | /*80158C64*/ { ppc::op_fcmpo(c, 0, 0, 21); } | /*80158C68*/ { ppc::op_stfs(c, 0, 25, 200); } | /*80158C6C*/ { { if (ppc::crbit(c, 0) == false) { goto L_80158CA0; } } }
- game_033.cpp:896 /*80158C70*/ { ppc::op_lfs(c, 0, 25, 200); } | /*80158C74*/ { ppc::op_fadds(c, 0, 0, 22, false); } | /*80158C78*/ { ppc::op_stfs(c, 0, 25, 200); } | /*80158C7C*/ { goto L_80158CA0; }
- game_033.cpp:904 /*80158C8C*/ { ppc::op_fmadds(c, 0, 2, 0, 1, false); } | /*80158C90*/ { ppc::op_fcmpo(c, 0, 0, 22); } | /*80158C94*/ { ppc::op_stfs(c, 0, 25, 200); } | /*80158C98*/ { { if (ppc::crbit(c, 1) == false) { goto L_80158CA0; } } }
- game_033.cpp:917 /*80158CBC*/ { ppc::op_fnmsubs(c, 0, 1, 0, 25, false); } | /*80158CC0*/ { ppc::op_fcmpo(c, 0, 0, 21); } | /*80158CC4*/ { ppc::op_stfs(c, 0, 25, 168); } | /*80158CC8*/ { { if (ppc::crbit(c, 0) == false) { goto L_80158CD8; } } }
- game_033.cpp:921 /*80158CCC*/ { ppc::op_lfs(c, 0, 25, 168); } | /*80158CD0*/ { ppc::op_fadds(c, 0, 0, 26, false); } | /*80158CD4*/ { ppc::op_stfs(c, 0, 25, 168); } | L_80158CD8:;
- game_033.cpp:930 /*80158CEC*/ { ppc::op_fnmsubs(c, 0, 28, 0, 1, false); } | /*80158CF0*/ { ppc::op_fcmpo(c, 0, 0, 21); } | /*80158CF4*/ { ppc::op_stfs(c, 0, 25, 184); } | /*80158CF8*/ { { if (ppc::crbit(c, 0) == false) { goto L_80158D08; } } }
- game_033.cpp:934 /*80158CFC*/ { ppc::op_lfs(c, 0, 25, 184); } | /*80158D00*/ { ppc::op_fadds(c, 0, 0, 26, false); } | /*80158D04*/ { ppc::op_stfs(c, 0, 25, 184); } | L_80158D08:;
- 80158C08 sole reader of806C6444; f24 used only at58CB8/58CE8, position direction from projectedpoint stack40/44/48. test_game_pacing30..480/pause.
## 80162B80 — функция 80162A68

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Trail history advances at nominal-frame boundaries; existing secondary-camera branch preserves projection on every call.

**Происхождение:** baseline-3513.

**Поле:** `r3@block16+0x24`. Тип: `integer-step`; операции: `addi`. Приоритет сканера: `medium`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_033.cpp:11887`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_033.cpp:11899 /*80162B78*/ { ppc::op_addi(c, 14, 0, 12); } | /*80162B7C*/ { ppc::op_addi(c, 0, 4, 1); } | /*80162B80*/ { ppc::op_stw(c, 0, 3, 36); } | /*80162B84*/ { goto L_80162BC0; }
- 801638C0 writes render-pass gate and dispatches virtual+8;8013D35C calls it per scene render;80162B44 gate precedes history shift4876/5116 and vertex rebuild,63204 projection always runs. hle_TrailHistoryStep;30..480FPS/pause/catch-up/secondary-pass/exception tests.
## 80189048 — функция 80188F74

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Texture-matrix translation phase adds 0.008 per render call; sole literal reader80189034 now receives step-scaled double at806C6B50 fromFrameWait.

**Происхождение:** baseline-3513.

**Поле:** `absolute+0x806C3D00`. Тип: `float-add`; операции: `fadd`. Приоритет сканера: `high`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_036.cpp:9889`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_036.cpp:9915 /*80189040*/ { ppc::op_frsp(c, 1, 1, false); } | /*80189044*/ { ppc::op_fcmpo(c, 0, 1, 0); } | /*80189048*/ { ppc::op_stfs(c, 1, 13, -26912); } | /*8018904C*/ { { if (ppc::crbit(c, 1) == false) { goto L_80189058; } } }
- 80188FE4..80189010 phase used inGXLoadTexMtxImm;80189030..054 increment/wrap; literal r2-23472=806C6B50 .008, threshold1.
## 801897C4 — функция 80189434

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Two persistent texture phase arrays integrate fixed per-draw affine speeds; scale their four unique literals by elapsed nominal frames.

**Происхождение:** baseline-3513.

**Поле:** `r19@block39+0x38`. Тип: `float-add`; операции: `fsubs`, `fmadds`, `fsubs`. Приоритет сканера: `inspect-loop`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_036.cpp:10398`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_036.cpp:10424 /*801897BC*/ { ppc::op_fsubs(c, 0, 0, 1, false); } | /*801897C0*/ { ppc::op_fcmpo(c, 0, 0, 25); } | /*801897C4*/ { ppc::op_stfs(c, 0, 19, 56); } | /*801897C8*/ { { if (ppc::crbit(c, 0) == false) { goto L_801897D4; } } }
- game_036.cpp:10448 /*80189818*/ { ppc::op_fsubs(c, 0, 0, 1, false); } | /*8018981C*/ { ppc::op_fcmpo(c, 0, 0, 25); } | /*80189820*/ { ppc::op_stfs(c, 0, 19, 96); } | /*80189824*/ { { if (ppc::crbit(c, 0) == false) { goto L_80189830; } } }
- 80189690/698/6A0/6A8 sole readers for806C6B90/6B8C/6B98/6B94. 897B8/BC/C4 phase56-=0.00056*layer+0.005;89814/18/20 phase96-=0.001*layer+0.01. Ten layers, constructor89258 seedsfields; originalwrap mathretained. Testalllayers30–480Hz,pause,30Hzrestoration.
## 80189820 — функция 80189434

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Two persistent texture phase arrays integrate fixed per-draw affine speeds; scale their four unique literals by elapsed nominal frames.

**Происхождение:** baseline-3513.

**Поле:** `r19@block42+0x60`. Тип: `float-add`; операции: `fsubs`, `fmadds`, `fsubs`. Приоритет сканера: `inspect-loop`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_036.cpp:10422`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_036.cpp:10424 /*801897BC*/ { ppc::op_fsubs(c, 0, 0, 1, false); } | /*801897C0*/ { ppc::op_fcmpo(c, 0, 0, 25); } | /*801897C4*/ { ppc::op_stfs(c, 0, 19, 56); } | /*801897C8*/ { { if (ppc::crbit(c, 0) == false) { goto L_801897D4; } } }
- game_036.cpp:10448 /*80189818*/ { ppc::op_fsubs(c, 0, 0, 1, false); } | /*8018981C*/ { ppc::op_fcmpo(c, 0, 0, 25); } | /*80189820*/ { ppc::op_stfs(c, 0, 19, 96); } | /*80189824*/ { { if (ppc::crbit(c, 0) == false) { goto L_80189830; } } }
- 80189690/698/6A0/6A8 sole readers for806C6B90/6B8C/6B98/6B94. 897B8/BC/C4 phase56-=0.00056*layer+0.005;89814/18/20 phase96-=0.001*layer+0.01. Ten layers, constructor89258 seedsfields; originalwrap mathretained. Testalllayers30–480Hz,pause,30Hzrestoration.
## 80192A08 — функция 801926B0

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** History sampling advances by (10*dt)/(20*dt)=0.5 per call; restore nominal-frame duration.

**Происхождение:** baseline-3513.

**Поле:** `r25@block60+0xDE4`. Тип: `float-add`; операции: `fsubs`. Приоритет сканера: `inspect-loop`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_037.cpp:8656`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_037.cpp:8872 /*80192A00*/ { ppc::op_addi(c, 21, 0, 240); } | /*80192A04*/ { ppc::op_fsubs(c, 0, 0, 25, false); } | /*80192A08*/ { ppc::op_stfs(c, 0, 25, 3556); } | L_80192A0C:; callers=80196434
- game_037.cpp:8966 /*80192B60*/ { ppc::op_fmuls(c, 0, 0, 1, false); } | /*80192B64*/ { ppc::op_fmadds(c, 0, 25, 2, 0, false); } | /*80192B68*/ { ppc::op_stfs(c, 0, 25, 5204); } | /*80192B6C*/ { c.lr = 0x80192B70u; fn_801AF03C(c); } callers=80196434
- game_037.cpp:8974 /*80192B80*/ { ppc::op_fmuls(c, 0, 0, 1, false); } | /*80192B84*/ { ppc::op_fmadds(c, 0, 25, 2, 0, false); } | /*80192B88*/ { ppc::op_stfs(c, 0, 25, 5208); } | /*80192B8C*/ { c.lr = 0x80192B90u; fn_801AF03C(c); } callers=80196434
- game_037.cpp:8979 /*80192B94*/ { ppc::op_fsubs(c, 0, 0, 1, false); } | /*80192B98*/ { ppc::op_fcmpo(c, 0, 0, 26); } | /*80192B9C*/ { ppc::op_stfs(c, 0, 19, 7600); } | /*80192BA0*/ { { if (ppc::crbit(c, 0) == false) { goto L_80192DBC; } } } callers=80196434
- 92728/3C/44 builds f27=20dt,f28=10dt;929D8 divides and929E8 addsphase3556;92A08 wrapsandsampleshistory. ModelHistoryIdentity onlyLR929B0 rebuildsf28=10dt*step_scale everychild; overwritten92B34 beforelateruses. Regressiontwochildren30–480Hz,unrelatedidentitycalls,pause.

**Прямые вызывающие функции:** `80196434`.
## 801A5164 — функция 801A5130

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Staggered action countdown consumes nominal-frame ticks, while regular actor update continues every render frame.

**Происхождение:** baseline-3513.

**Поле:** `r31@block4+0x2128`. Тип: `integer-step`; операции: `addi`. Приоритет сканера: `medium`.

**Исходная привязка:** `generated/generation_8f5c8a9f29ed8c00/game_039.cpp:6026`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_039.cpp:6026 801A5164 r31@block4+0x2128 addi /*801A5164*/ { ppc::op_stw(c, 0, 31, 8488); } callers=
- A1470 initializes nearby actors+8488 to sequential1..N atA15B4;A4C40 resets-1;A5130 decrements once/update and calls800ACB54 atzero. DeferredActorStep replays only countdown block pernominaltick and continues tailA5168 eachcall. Tests30..480Hz,pausedzero,multi-tickcatchup,one-shot,stack/LR.
## 801B9294 — функция 801B916C

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Effect retirement grace counter now advances in nominal 30 Hz frames, while original object scanning and animation completion checks still run each update.

**Происхождение:** baseline-3513.

**Поле:** `r28@block23+0x108`. Тип: `integer-step`; операции: `addi`. Приоритет сканера: `inspect-loop`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_042.cpp:10593`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_042.cpp:10668 801B9294 r28@block23+0x108 addi /*801B9294*/ { ppc::op_stw(c, 0, 28, 264); } callers=
- game_042.cpp:10724 801B9360 r5@block42+0x5978 addi /*801B9360*/ { ppc::op_stw(c, 0, 5, 22904); } callers=
- 801B9220..801B9294 counter gates completion check until age >= 2; hle_EffectRetireStep snapshots record identity and adjusts only unchanged original increments; 30..480 Hz, pause, replacement and grace-threshold regression passed
## 801C478C — функция 801C44BC

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** FlightPathStep already normalizes incoming displacement in modes lacking dt; remaining+36 is distance/pathlength progress.

**Происхождение:** baseline-3513.

**Поле:** `r31@block50+0x24`. Тип: `float-add`; операции: `fdivs`, `fadds`. Приоритет сканера: `high`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_044.cpp:4153`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_044.cpp:4181 801C478C r31@block51+0x24 fdivs/fadds /*801C478C*/ { ppc::op_stfs(c, 1, 31, 36); } callers=
- game/game_pacing.cpp hle_FlightPathStep
- 801C4778..78C
## 801C57B8 — функция 801C56F4

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Original body is entered via FlightPhaseStep; states32/34 speed is normalized.

**Происхождение:** baseline-3513.

**Поле:** `r3@block17+0x2010`. Тип: `float-add`; операции: `fadds`. Приоритет сканера: `high`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_044.cpp:5402`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_044.cpp:5388 801C5720 r3@block0+0x21E0 fadds /*801C5720*/ { ppc::op_stfs(c, 0, 3, 8672); } callers=
- game_044.cpp:5418 801C578C r3@block13+0x2010 fadds /*801C578C*/ { ppc::op_stfs(c, 1, 3, 8208); } callers=
- game_044.cpp:5430 801C57B8 r3@block17+0x2010 fadds /*801C57B8*/ { ppc::op_stfs(c, 1, 3, 8208); } callers=
- hle_FlightPhaseStep normalizes +8176 for states32/34 before 801C56F4.
## 801C5C94 — функция 801C5B48

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Distance remaining consumes displacement normalized by existing FlightStep hook.

**Происхождение:** baseline-3513.

**Поле:** `r30@block22+0x2010`. Тип: `float-add`; операции: `fnmsubs`. Приоритет сканера: `high`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_044.cpp:5740`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_044.cpp:5768 801C5C94 r30@block22+0x2010 fnmsubs /*801C5C94*/ { ppc::op_stfs(c, 1, 30, 8208); } callers=801C4F14,801C60C4
- 801C5C30 FlightStep(stack+8); 801C5C80..94 +8208 -= length(direction)*stack[8].

**Прямые вызывающие функции:** `801C4F14`, `801C60C4`.
## 801F2104 — функция 801F1B88

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Model history sample phase advances .2 per update; scaled sole-reader literal806C8A18 preserves six nominal samples/sec.

**Происхождение:** baseline-3513.

**Поле:** `r26@block111+0xE3C`. Тип: `float-add`; операции: `fsubs`. Приоритет сканера: `inspect-loop`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_051.cpp:4778`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_051.cpp:4799 801F2104 r26@block111+0xE3C fsubs /*801F2104*/ { ppc::op_stfs(c, 0, 26, 3644); } caller=801F4D90
- game_051.cpp:4893 801F2264 r25@block133+0x14AC fsubs/fmuls/fmadds /*801F2264*/ { ppc::op_stfs(c, 0, 25, 5292); } caller=801F4D90
- game_051.cpp:4901 801F2284 r25@block134+0x14B0 fsubs/fmuls/fmadds /*801F2284*/ { ppc::op_stfs(c, 0, 25, 5296); } caller=801F4D90
- game_051.cpp:4906 801F2298 r19@block135+0x1688 fsubs /*801F2298*/ { ppc::op_stfs(c, 0, 19, 5768); } caller=801F4D90
- 801F2020 load;20E0..20EC increment;2100..217C whilephase>1 shifts48-byte history and samplesmodel;21A0..21F8 interpolatehistory.

**Прямые вызывающие функции:** `801F4D90`.
## 802102FC — функция 8021027C

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Known PropStep wrapper schedules the retained per-call spring solver at nominal 60 Hz; this finding is covered by that targeted fix.

**Происхождение:** baseline-3513.

**Поле:** `r31@block5+0x54`. Тип: `integer-step`; операции: `addi`. Приоритет сканера: `inspect-loop`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_055.cpp:5448`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_055.cpp:5455 802102FC r31@block5+0x54 addi /*802102FC*/ { ppc::op_stw(c, 3, 31, 84); } caller=
- game_055.cpp:5507 802103B4 r31@block20+0x2C fmuls /*802103B4*/ { ppc::op_stfs(c, 1, 31, 44); } caller=
- game_055.cpp:5528 802103FC r31@block26+0x30 fmuls /*802103FC*/ { ppc::op_stfs(c, 1, 31, 48); } caller=
- game_055.cpp:5549 80210444 r31@block32+0x34 fmuls /*80210444*/ { ppc::op_stfs(c, 1, 31, 52); } caller=
- game_055.cpp:5572 80210498 r31@block35+0x38 fmuls /*80210498*/ { ppc::op_stfs(c, 2, 31, 56); } caller=
- game_055.cpp:5573 8021049C r31@block35+0x3C fmuls /*8021049C*/ { ppc::op_stfs(c, 1, 31, 60); } caller=
- game_055.cpp:5574 802104A0 r31@block35+0x40 fmuls /*802104A0*/ { ppc::op_stfs(c, 0, 31, 64); } caller=
- game_055.cpp:5589 802104D8 r31@block38+0x38 fmadds /*802104D8*/ { ppc::op_stfs(c, 4, 31, 56); } caller=
- game_055.cpp:5591 802104E0 r31@block38+0x3C fmadds /*802104E0*/ { ppc::op_stfs(c, 2, 31, 60); } caller=
- game_055.cpp:5592 802104E4 r31@block38+0x40 fmadds /*802104E4*/ { ppc::op_stfs(c, 0, 31, 64); } caller=
- game_055.cpp:5603 8021050C r31@block39+0x44 fmuls /*8021050C*/ { ppc::op_stfs(c, 2, 31, 68); } caller=
## 802103B4 — функция 8021027C

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Known PropStep wrapper schedules the retained per-call spring solver at nominal 60 Hz; this finding is covered by that targeted fix.

**Происхождение:** baseline-3513.

**Поле:** `r31@block20+0x2C`. Тип: `float-multiply`; операции: `fmuls`. Приоритет сканера: `inspect-loop`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_055.cpp:5500`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_055.cpp:5455 802102FC r31@block5+0x54 addi /*802102FC*/ { ppc::op_stw(c, 3, 31, 84); } caller=
- game_055.cpp:5507 802103B4 r31@block20+0x2C fmuls /*802103B4*/ { ppc::op_stfs(c, 1, 31, 44); } caller=
- game_055.cpp:5528 802103FC r31@block26+0x30 fmuls /*802103FC*/ { ppc::op_stfs(c, 1, 31, 48); } caller=
- game_055.cpp:5549 80210444 r31@block32+0x34 fmuls /*80210444*/ { ppc::op_stfs(c, 1, 31, 52); } caller=
- game_055.cpp:5572 80210498 r31@block35+0x38 fmuls /*80210498*/ { ppc::op_stfs(c, 2, 31, 56); } caller=
- game_055.cpp:5573 8021049C r31@block35+0x3C fmuls /*8021049C*/ { ppc::op_stfs(c, 1, 31, 60); } caller=
- game_055.cpp:5574 802104A0 r31@block35+0x40 fmuls /*802104A0*/ { ppc::op_stfs(c, 0, 31, 64); } caller=
- game_055.cpp:5589 802104D8 r31@block38+0x38 fmadds /*802104D8*/ { ppc::op_stfs(c, 4, 31, 56); } caller=
- game_055.cpp:5591 802104E0 r31@block38+0x3C fmadds /*802104E0*/ { ppc::op_stfs(c, 2, 31, 60); } caller=
- game_055.cpp:5592 802104E4 r31@block38+0x40 fmadds /*802104E4*/ { ppc::op_stfs(c, 0, 31, 64); } caller=
- game_055.cpp:5603 8021050C r31@block39+0x44 fmuls /*8021050C*/ { ppc::op_stfs(c, 2, 31, 68); } caller=
## 802103FC — функция 8021027C

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Known PropStep wrapper schedules the retained per-call spring solver at nominal 60 Hz; this finding is covered by that targeted fix.

**Происхождение:** baseline-3513.

**Поле:** `r31@block26+0x30`. Тип: `float-multiply`; операции: `fmuls`. Приоритет сканера: `inspect-loop`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_055.cpp:5521`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_055.cpp:5455 802102FC r31@block5+0x54 addi /*802102FC*/ { ppc::op_stw(c, 3, 31, 84); } caller=
- game_055.cpp:5507 802103B4 r31@block20+0x2C fmuls /*802103B4*/ { ppc::op_stfs(c, 1, 31, 44); } caller=
- game_055.cpp:5528 802103FC r31@block26+0x30 fmuls /*802103FC*/ { ppc::op_stfs(c, 1, 31, 48); } caller=
- game_055.cpp:5549 80210444 r31@block32+0x34 fmuls /*80210444*/ { ppc::op_stfs(c, 1, 31, 52); } caller=
- game_055.cpp:5572 80210498 r31@block35+0x38 fmuls /*80210498*/ { ppc::op_stfs(c, 2, 31, 56); } caller=
- game_055.cpp:5573 8021049C r31@block35+0x3C fmuls /*8021049C*/ { ppc::op_stfs(c, 1, 31, 60); } caller=
- game_055.cpp:5574 802104A0 r31@block35+0x40 fmuls /*802104A0*/ { ppc::op_stfs(c, 0, 31, 64); } caller=
- game_055.cpp:5589 802104D8 r31@block38+0x38 fmadds /*802104D8*/ { ppc::op_stfs(c, 4, 31, 56); } caller=
- game_055.cpp:5591 802104E0 r31@block38+0x3C fmadds /*802104E0*/ { ppc::op_stfs(c, 2, 31, 60); } caller=
- game_055.cpp:5592 802104E4 r31@block38+0x40 fmadds /*802104E4*/ { ppc::op_stfs(c, 0, 31, 64); } caller=
- game_055.cpp:5603 8021050C r31@block39+0x44 fmuls /*8021050C*/ { ppc::op_stfs(c, 2, 31, 68); } caller=
## 80210444 — функция 8021027C

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Known PropStep wrapper schedules the retained per-call spring solver at nominal 60 Hz; this finding is covered by that targeted fix.

**Происхождение:** baseline-3513.

**Поле:** `r31@block32+0x34`. Тип: `float-multiply`; операции: `fmuls`. Приоритет сканера: `inspect-loop`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_055.cpp:5542`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_055.cpp:5455 802102FC r31@block5+0x54 addi /*802102FC*/ { ppc::op_stw(c, 3, 31, 84); } caller=
- game_055.cpp:5507 802103B4 r31@block20+0x2C fmuls /*802103B4*/ { ppc::op_stfs(c, 1, 31, 44); } caller=
- game_055.cpp:5528 802103FC r31@block26+0x30 fmuls /*802103FC*/ { ppc::op_stfs(c, 1, 31, 48); } caller=
- game_055.cpp:5549 80210444 r31@block32+0x34 fmuls /*80210444*/ { ppc::op_stfs(c, 1, 31, 52); } caller=
- game_055.cpp:5572 80210498 r31@block35+0x38 fmuls /*80210498*/ { ppc::op_stfs(c, 2, 31, 56); } caller=
- game_055.cpp:5573 8021049C r31@block35+0x3C fmuls /*8021049C*/ { ppc::op_stfs(c, 1, 31, 60); } caller=
- game_055.cpp:5574 802104A0 r31@block35+0x40 fmuls /*802104A0*/ { ppc::op_stfs(c, 0, 31, 64); } caller=
- game_055.cpp:5589 802104D8 r31@block38+0x38 fmadds /*802104D8*/ { ppc::op_stfs(c, 4, 31, 56); } caller=
- game_055.cpp:5591 802104E0 r31@block38+0x3C fmadds /*802104E0*/ { ppc::op_stfs(c, 2, 31, 60); } caller=
- game_055.cpp:5592 802104E4 r31@block38+0x40 fmadds /*802104E4*/ { ppc::op_stfs(c, 0, 31, 64); } caller=
- game_055.cpp:5603 8021050C r31@block39+0x44 fmuls /*8021050C*/ { ppc::op_stfs(c, 2, 31, 68); } caller=
## 80210498 — функция 8021027C

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Known PropStep wrapper schedules the retained per-call spring solver at nominal 60 Hz; this finding is covered by that targeted fix.

**Происхождение:** baseline-3513.

**Поле:** `r31@block35+0x38`. Тип: `float-multiply`; операции: `fmuls`. Приоритет сканера: `inspect-loop`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_055.cpp:5565`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_055.cpp:5455 802102FC r31@block5+0x54 addi /*802102FC*/ { ppc::op_stw(c, 3, 31, 84); } caller=
- game_055.cpp:5507 802103B4 r31@block20+0x2C fmuls /*802103B4*/ { ppc::op_stfs(c, 1, 31, 44); } caller=
- game_055.cpp:5528 802103FC r31@block26+0x30 fmuls /*802103FC*/ { ppc::op_stfs(c, 1, 31, 48); } caller=
- game_055.cpp:5549 80210444 r31@block32+0x34 fmuls /*80210444*/ { ppc::op_stfs(c, 1, 31, 52); } caller=
- game_055.cpp:5572 80210498 r31@block35+0x38 fmuls /*80210498*/ { ppc::op_stfs(c, 2, 31, 56); } caller=
- game_055.cpp:5573 8021049C r31@block35+0x3C fmuls /*8021049C*/ { ppc::op_stfs(c, 1, 31, 60); } caller=
- game_055.cpp:5574 802104A0 r31@block35+0x40 fmuls /*802104A0*/ { ppc::op_stfs(c, 0, 31, 64); } caller=
- game_055.cpp:5589 802104D8 r31@block38+0x38 fmadds /*802104D8*/ { ppc::op_stfs(c, 4, 31, 56); } caller=
- game_055.cpp:5591 802104E0 r31@block38+0x3C fmadds /*802104E0*/ { ppc::op_stfs(c, 2, 31, 60); } caller=
- game_055.cpp:5592 802104E4 r31@block38+0x40 fmadds /*802104E4*/ { ppc::op_stfs(c, 0, 31, 64); } caller=
- game_055.cpp:5603 8021050C r31@block39+0x44 fmuls /*8021050C*/ { ppc::op_stfs(c, 2, 31, 68); } caller=
## 8021049C — функция 8021027C

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Known PropStep wrapper schedules the retained per-call spring solver at nominal 60 Hz; this finding is covered by that targeted fix.

**Происхождение:** baseline-3513.

**Поле:** `r31@block35+0x3C`. Тип: `float-multiply`; операции: `fmuls`. Приоритет сканера: `inspect-loop`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_055.cpp:5566`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_055.cpp:5455 802102FC r31@block5+0x54 addi /*802102FC*/ { ppc::op_stw(c, 3, 31, 84); } caller=
- game_055.cpp:5507 802103B4 r31@block20+0x2C fmuls /*802103B4*/ { ppc::op_stfs(c, 1, 31, 44); } caller=
- game_055.cpp:5528 802103FC r31@block26+0x30 fmuls /*802103FC*/ { ppc::op_stfs(c, 1, 31, 48); } caller=
- game_055.cpp:5549 80210444 r31@block32+0x34 fmuls /*80210444*/ { ppc::op_stfs(c, 1, 31, 52); } caller=
- game_055.cpp:5572 80210498 r31@block35+0x38 fmuls /*80210498*/ { ppc::op_stfs(c, 2, 31, 56); } caller=
- game_055.cpp:5573 8021049C r31@block35+0x3C fmuls /*8021049C*/ { ppc::op_stfs(c, 1, 31, 60); } caller=
- game_055.cpp:5574 802104A0 r31@block35+0x40 fmuls /*802104A0*/ { ppc::op_stfs(c, 0, 31, 64); } caller=
- game_055.cpp:5589 802104D8 r31@block38+0x38 fmadds /*802104D8*/ { ppc::op_stfs(c, 4, 31, 56); } caller=
- game_055.cpp:5591 802104E0 r31@block38+0x3C fmadds /*802104E0*/ { ppc::op_stfs(c, 2, 31, 60); } caller=
- game_055.cpp:5592 802104E4 r31@block38+0x40 fmadds /*802104E4*/ { ppc::op_stfs(c, 0, 31, 64); } caller=
- game_055.cpp:5603 8021050C r31@block39+0x44 fmuls /*8021050C*/ { ppc::op_stfs(c, 2, 31, 68); } caller=
## 802104A0 — функция 8021027C

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Known PropStep wrapper schedules the retained per-call spring solver at nominal 60 Hz; this finding is covered by that targeted fix.

**Происхождение:** baseline-3513.

**Поле:** `r31@block35+0x40`. Тип: `float-multiply`; операции: `fmuls`. Приоритет сканера: `inspect-loop`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_055.cpp:5567`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_055.cpp:5455 802102FC r31@block5+0x54 addi /*802102FC*/ { ppc::op_stw(c, 3, 31, 84); } caller=
- game_055.cpp:5507 802103B4 r31@block20+0x2C fmuls /*802103B4*/ { ppc::op_stfs(c, 1, 31, 44); } caller=
- game_055.cpp:5528 802103FC r31@block26+0x30 fmuls /*802103FC*/ { ppc::op_stfs(c, 1, 31, 48); } caller=
- game_055.cpp:5549 80210444 r31@block32+0x34 fmuls /*80210444*/ { ppc::op_stfs(c, 1, 31, 52); } caller=
- game_055.cpp:5572 80210498 r31@block35+0x38 fmuls /*80210498*/ { ppc::op_stfs(c, 2, 31, 56); } caller=
- game_055.cpp:5573 8021049C r31@block35+0x3C fmuls /*8021049C*/ { ppc::op_stfs(c, 1, 31, 60); } caller=
- game_055.cpp:5574 802104A0 r31@block35+0x40 fmuls /*802104A0*/ { ppc::op_stfs(c, 0, 31, 64); } caller=
- game_055.cpp:5589 802104D8 r31@block38+0x38 fmadds /*802104D8*/ { ppc::op_stfs(c, 4, 31, 56); } caller=
- game_055.cpp:5591 802104E0 r31@block38+0x3C fmadds /*802104E0*/ { ppc::op_stfs(c, 2, 31, 60); } caller=
- game_055.cpp:5592 802104E4 r31@block38+0x40 fmadds /*802104E4*/ { ppc::op_stfs(c, 0, 31, 64); } caller=
- game_055.cpp:5603 8021050C r31@block39+0x44 fmuls /*8021050C*/ { ppc::op_stfs(c, 2, 31, 68); } caller=
## 802104D8 — функция 8021027C

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Known PropStep wrapper schedules the retained per-call spring solver at nominal 60 Hz; this finding is covered by that targeted fix.

**Происхождение:** baseline-3513.

**Поле:** `r31@block38+0x38`. Тип: `float-add`; операции: `fmadds`. Приоритет сканера: `inspect-loop`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_055.cpp:5582`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_055.cpp:5455 802102FC r31@block5+0x54 addi /*802102FC*/ { ppc::op_stw(c, 3, 31, 84); } caller=
- game_055.cpp:5507 802103B4 r31@block20+0x2C fmuls /*802103B4*/ { ppc::op_stfs(c, 1, 31, 44); } caller=
- game_055.cpp:5528 802103FC r31@block26+0x30 fmuls /*802103FC*/ { ppc::op_stfs(c, 1, 31, 48); } caller=
- game_055.cpp:5549 80210444 r31@block32+0x34 fmuls /*80210444*/ { ppc::op_stfs(c, 1, 31, 52); } caller=
- game_055.cpp:5572 80210498 r31@block35+0x38 fmuls /*80210498*/ { ppc::op_stfs(c, 2, 31, 56); } caller=
- game_055.cpp:5573 8021049C r31@block35+0x3C fmuls /*8021049C*/ { ppc::op_stfs(c, 1, 31, 60); } caller=
- game_055.cpp:5574 802104A0 r31@block35+0x40 fmuls /*802104A0*/ { ppc::op_stfs(c, 0, 31, 64); } caller=
- game_055.cpp:5589 802104D8 r31@block38+0x38 fmadds /*802104D8*/ { ppc::op_stfs(c, 4, 31, 56); } caller=
- game_055.cpp:5591 802104E0 r31@block38+0x3C fmadds /*802104E0*/ { ppc::op_stfs(c, 2, 31, 60); } caller=
- game_055.cpp:5592 802104E4 r31@block38+0x40 fmadds /*802104E4*/ { ppc::op_stfs(c, 0, 31, 64); } caller=
- game_055.cpp:5603 8021050C r31@block39+0x44 fmuls /*8021050C*/ { ppc::op_stfs(c, 2, 31, 68); } caller=
## 802104E0 — функция 8021027C

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Known PropStep wrapper schedules the retained per-call spring solver at nominal 60 Hz; this finding is covered by that targeted fix.

**Происхождение:** baseline-3513.

**Поле:** `r31@block38+0x3C`. Тип: `float-add`; операции: `fmadds`. Приоритет сканера: `inspect-loop`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_055.cpp:5584`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_055.cpp:5455 802102FC r31@block5+0x54 addi /*802102FC*/ { ppc::op_stw(c, 3, 31, 84); } caller=
- game_055.cpp:5507 802103B4 r31@block20+0x2C fmuls /*802103B4*/ { ppc::op_stfs(c, 1, 31, 44); } caller=
- game_055.cpp:5528 802103FC r31@block26+0x30 fmuls /*802103FC*/ { ppc::op_stfs(c, 1, 31, 48); } caller=
- game_055.cpp:5549 80210444 r31@block32+0x34 fmuls /*80210444*/ { ppc::op_stfs(c, 1, 31, 52); } caller=
- game_055.cpp:5572 80210498 r31@block35+0x38 fmuls /*80210498*/ { ppc::op_stfs(c, 2, 31, 56); } caller=
- game_055.cpp:5573 8021049C r31@block35+0x3C fmuls /*8021049C*/ { ppc::op_stfs(c, 1, 31, 60); } caller=
- game_055.cpp:5574 802104A0 r31@block35+0x40 fmuls /*802104A0*/ { ppc::op_stfs(c, 0, 31, 64); } caller=
- game_055.cpp:5589 802104D8 r31@block38+0x38 fmadds /*802104D8*/ { ppc::op_stfs(c, 4, 31, 56); } caller=
- game_055.cpp:5591 802104E0 r31@block38+0x3C fmadds /*802104E0*/ { ppc::op_stfs(c, 2, 31, 60); } caller=
- game_055.cpp:5592 802104E4 r31@block38+0x40 fmadds /*802104E4*/ { ppc::op_stfs(c, 0, 31, 64); } caller=
- game_055.cpp:5603 8021050C r31@block39+0x44 fmuls /*8021050C*/ { ppc::op_stfs(c, 2, 31, 68); } caller=
## 802104E4 — функция 8021027C

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Known PropStep wrapper schedules the retained per-call spring solver at nominal 60 Hz; this finding is covered by that targeted fix.

**Происхождение:** baseline-3513.

**Поле:** `r31@block38+0x40`. Тип: `float-add`; операции: `fmadds`. Приоритет сканера: `inspect-loop`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_055.cpp:5585`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_055.cpp:5455 802102FC r31@block5+0x54 addi /*802102FC*/ { ppc::op_stw(c, 3, 31, 84); } caller=
- game_055.cpp:5507 802103B4 r31@block20+0x2C fmuls /*802103B4*/ { ppc::op_stfs(c, 1, 31, 44); } caller=
- game_055.cpp:5528 802103FC r31@block26+0x30 fmuls /*802103FC*/ { ppc::op_stfs(c, 1, 31, 48); } caller=
- game_055.cpp:5549 80210444 r31@block32+0x34 fmuls /*80210444*/ { ppc::op_stfs(c, 1, 31, 52); } caller=
- game_055.cpp:5572 80210498 r31@block35+0x38 fmuls /*80210498*/ { ppc::op_stfs(c, 2, 31, 56); } caller=
- game_055.cpp:5573 8021049C r31@block35+0x3C fmuls /*8021049C*/ { ppc::op_stfs(c, 1, 31, 60); } caller=
- game_055.cpp:5574 802104A0 r31@block35+0x40 fmuls /*802104A0*/ { ppc::op_stfs(c, 0, 31, 64); } caller=
- game_055.cpp:5589 802104D8 r31@block38+0x38 fmadds /*802104D8*/ { ppc::op_stfs(c, 4, 31, 56); } caller=
- game_055.cpp:5591 802104E0 r31@block38+0x3C fmadds /*802104E0*/ { ppc::op_stfs(c, 2, 31, 60); } caller=
- game_055.cpp:5592 802104E4 r31@block38+0x40 fmadds /*802104E4*/ { ppc::op_stfs(c, 0, 31, 64); } caller=
- game_055.cpp:5603 8021050C r31@block39+0x44 fmuls /*8021050C*/ { ppc::op_stfs(c, 2, 31, 68); } caller=
## 8021050C — функция 8021027C

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Known PropStep wrapper schedules the retained per-call spring solver at nominal 60 Hz; this finding is covered by that targeted fix.

**Происхождение:** baseline-3513.

**Поле:** `r31@block39+0x44`. Тип: `float-multiply`; операции: `fmuls`. Приоритет сканера: `inspect-loop`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_055.cpp:5596`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_055.cpp:5455 802102FC r31@block5+0x54 addi /*802102FC*/ { ppc::op_stw(c, 3, 31, 84); } caller=
- game_055.cpp:5507 802103B4 r31@block20+0x2C fmuls /*802103B4*/ { ppc::op_stfs(c, 1, 31, 44); } caller=
- game_055.cpp:5528 802103FC r31@block26+0x30 fmuls /*802103FC*/ { ppc::op_stfs(c, 1, 31, 48); } caller=
- game_055.cpp:5549 80210444 r31@block32+0x34 fmuls /*80210444*/ { ppc::op_stfs(c, 1, 31, 52); } caller=
- game_055.cpp:5572 80210498 r31@block35+0x38 fmuls /*80210498*/ { ppc::op_stfs(c, 2, 31, 56); } caller=
- game_055.cpp:5573 8021049C r31@block35+0x3C fmuls /*8021049C*/ { ppc::op_stfs(c, 1, 31, 60); } caller=
- game_055.cpp:5574 802104A0 r31@block35+0x40 fmuls /*802104A0*/ { ppc::op_stfs(c, 0, 31, 64); } caller=
- game_055.cpp:5589 802104D8 r31@block38+0x38 fmadds /*802104D8*/ { ppc::op_stfs(c, 4, 31, 56); } caller=
- game_055.cpp:5591 802104E0 r31@block38+0x3C fmadds /*802104E0*/ { ppc::op_stfs(c, 2, 31, 60); } caller=
- game_055.cpp:5592 802104E4 r31@block38+0x40 fmadds /*802104E4*/ { ppc::op_stfs(c, 0, 31, 64); } caller=
- game_055.cpp:5603 8021050C r31@block39+0x44 fmuls /*8021050C*/ { ppc::op_stfs(c, 2, 31, 68); } caller=
## 8021179C — функция 80211734

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Timeline+42312 advanced by one per invocation before comparing scripted event frame times; native prologue now adds game_step_scale before original comparisons.

**Происхождение:** baseline-3513.

**Поле:** `r3@block4+0xA548`. Тип: `float-add`; операции: `fadds`. Приоритет сканера: `high`.

**Исходная привязка:** `generated/generation_8f5c8a9f29ed8c00/game_056.cpp:96`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- game_056.cpp:96 8021179C r3@block4+0xA548 fadds /*8021179C*/ { ppc::op_stfs(c, 0, 26, -23224); } caller=
- 80211770/84/9C advance;80211808..2C event threshold;historical roof probe805BDA08 in F5;current map TimelineStep entry80211788;test30..480 events ABI pause
## 802448B4 — функция 802447D8

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** UI vertical velocity received -98/target each rendered update while position already uses dt; frame-boundary gravity literal now scales by nominal step. Adjacent configured angular drag is scaled in UiPhysicsStep.

**Происхождение:** baseline-3513.

**Поле:** `r3@block8+0x38`. Тип: `float-add`; операции: `fsubs`, `fdivs`, `fadds`. Приоритет сканера: `high`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_062.cpp:6019`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- 80244884 -> 8024488C -> 80244898 -> 8024489C -> 802448A0 -> 802448AC -> 802448B0 -> 802448B4
- 80244B84 -> 80244B90 -> 80244B94 -> 80244B9C
- 8024488C/AC/B4;80244960/64 movement dt;802454F0 config loader writes108 at80245574;802448F8/80244928 drag
## 802448F8 — функция 802447D8

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Angular drag is a per-call decrement; UiPhysicsStep temporarily scales configured drag+108 and restores its exact bits.

**Происхождение:** manual-call-chain.

**Доказательства / связанные адреса:**

- 802454F0
- 802448C4-80244938
- game/game_pacing.cpp:hle_UiPhysicsStep
## 80244928 — функция 802447D8

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Angular drag is a per-call decrement; UiPhysicsStep temporarily scales configured drag+108 and restores its exact bits.

**Происхождение:** manual-call-chain.

**Доказательства / связанные адреса:**

- 802454F0
- 802448C4-80244938
- game/game_pacing.cpp:hle_UiPhysicsStep
## 8039C288 — функция 8039C1D0

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Frame-based voice stop delays and volume/pause fades were outside the local recurrence scan. SoundFrameStep normalizes the whole locked sound-manager list walk803AA7B0; backend AX callbacks retain their independent3ms clock.

**Происхождение:** manual-call-chain.

**Доказательства / связанные адреса:**

- 8003A680 ->8004A3CC ->803A8E80 ->803AA7B0 ->virtual16=8039C1D0
- game/game_pacing.cpp:hle_SoundFrameStep
- runtime/ax_audio.cpp
- tests/test_game_pacing.cpp
## 8039C2F8 — функция 8039C1D0

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Frame-based voice stop delays and volume/pause fades were outside the local recurrence scan. SoundFrameStep normalizes the whole locked sound-manager list walk803AA7B0; backend AX callbacks retain their independent3ms clock.

**Происхождение:** manual-call-chain.

**Доказательства / связанные адреса:**

- 8003A680 ->8004A3CC ->803A8E80 ->803AA7B0 ->virtual16=8039C1D0
- game/game_pacing.cpp:hle_SoundFrameStep
- runtime/ax_audio.cpp
- tests/test_game_pacing.cpp
## 8039C36C — функция 8039C1D0

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Frame-based voice stop delays and volume/pause fades were outside the local recurrence scan. SoundFrameStep normalizes the whole locked sound-manager list walk803AA7B0; backend AX callbacks retain their independent3ms clock.

**Происхождение:** manual-call-chain.

**Доказательства / связанные адреса:**

- 8003A680 ->8004A3CC ->803A8E80 ->803AA7B0 ->virtual16=8039C1D0
- game/game_pacing.cpp:hle_SoundFrameStep
- runtime/ax_audio.cpp
- tests/test_game_pacing.cpp
## 8039C388 — функция 8039C1D0

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Frame-based voice stop delays and volume/pause fades were outside the local recurrence scan. SoundFrameStep normalizes the whole locked sound-manager list walk803AA7B0; backend AX callbacks retain their independent3ms clock.

**Происхождение:** manual-call-chain.

**Доказательства / связанные адреса:**

- 8003A680 ->8004A3CC ->803A8E80 ->803AA7B0 ->virtual16=8039C1D0
- game/game_pacing.cpp:hle_SoundFrameStep
- runtime/ax_audio.cpp
- tests/test_game_pacing.cpp
## 8039C3A0 — функция 8039C1D0

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Frame-based voice stop delays and volume/pause fades were outside the local recurrence scan. SoundFrameStep normalizes the whole locked sound-manager list walk803AA7B0; backend AX callbacks retain their independent3ms clock.

**Происхождение:** manual-call-chain.

**Доказательства / связанные адреса:**

- 8003A680 ->8004A3CC ->803A8E80 ->803AA7B0 ->virtual16=8039C1D0
- game/game_pacing.cpp:hle_SoundFrameStep
- runtime/ax_audio.cpp
- tests/test_game_pacing.cpp
## 803C233C — функция 803C1BFC

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Home menu timeout state machine now runs at nominal cadence through hle_HomeMenuStep, preserving input edges. Additional Home menu countdown is covered by the complete state-machine cadence hook; controller and fade child updates share the same entry.

**Происхождение:** addic-extension-57, manual-call-chain.

**Поле:** `r28@block153+0x1E0`. Тип: `integer-step`; операции: `addic_`. Приоритет сканера: `medium`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_103.cpp:5969`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- 803C2334-40 decrements menu field+480
- docs/testing/timing-triage.json supplemental finding for 803C233C
- game/game_pacing.cpp:hle_HomeMenuStep; tests/test_game_pacing.cpp
- 803C1BF8
- game/game_pacing.cpp:hle_HomeMenuStep
## 803C23EC — функция 803C1BF8

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Home menu timeout state machine was called once per unlocked render frame. HomeMenuStep runs the complete state machine at nominal cadence and preserves input edges across skipped frames. Initialization with null input still executes once.

**Происхождение:** baseline-3513.

**Поле:** `r28@block168+0x1E4`. Тип: `integer-step`; операции: `addi`. Приоритет сканера: `medium`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_103.cpp:6017`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- 803C23E4 -> 803C23E8 -> 803C23EC
- 803C25D0 -> 803C25D8 -> 803C25E0
- 803C25F8 -> 803C2600 -> 803C2608
- 803C261C -> 803C2624 -> 803C262C
- 801FFA04-801FFA24
- 800A7AF8-800A7AFC
- 803C01E8
- game/game_pacing.cpp:hle_HomeMenuStep
- tests/test_game_pacing.cpp
## 803C25E0 — функция 803C1BF8

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Home menu timeout state machine was called once per unlocked render frame. HomeMenuStep runs the complete state machine at nominal cadence and preserves input edges across skipped frames. Initialization with null input still executes once.

**Происхождение:** baseline-3513.

**Поле:** `r28@block205+0x94`. Тип: `integer-step`; операции: `addi`. Приоритет сканера: `medium`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_103.cpp:6152`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- 803C23E4 -> 803C23E8 -> 803C23EC
- 803C25D0 -> 803C25D8 -> 803C25E0
- 803C25F8 -> 803C2600 -> 803C2608
- 803C261C -> 803C2624 -> 803C262C
- 801FFA04-801FFA24
- 800A7AF8-800A7AFC
- 803C01E8
- game/game_pacing.cpp:hle_HomeMenuStep
- tests/test_game_pacing.cpp
## 803C2608 — функция 803C1BF8

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Home menu timeout state machine was called once per unlocked render frame. HomeMenuStep runs the complete state machine at nominal cadence and preserves input edges across skipped frames. Initialization with null input still executes once.

**Происхождение:** baseline-3513.

**Поле:** `r28@block208+0x18`. Тип: `integer-step`; операции: `addi`. Приоритет сканера: `medium`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_103.cpp:6163`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- 803C23E4 -> 803C23E8 -> 803C23EC
- 803C25D0 -> 803C25D8 -> 803C25E0
- 803C25F8 -> 803C2600 -> 803C2608
- 803C261C -> 803C2624 -> 803C262C
- 801FFA04-801FFA24
- 800A7AF8-800A7AFC
- 803C01E8
- game/game_pacing.cpp:hle_HomeMenuStep
- tests/test_game_pacing.cpp
## 803C262C — функция 803C1BF8

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Home menu timeout state machine was called once per unlocked render frame. HomeMenuStep runs the complete state machine at nominal cadence and preserves input edges across skipped frames. Initialization with null input still executes once.

**Происхождение:** baseline-3513.

**Поле:** `r28@block211+0x18`. Тип: `integer-step`; операции: `addi`. Приоритет сканера: `medium`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_103.cpp:6173`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- 803C23E4 -> 803C23E8 -> 803C23EC
- 803C25D0 -> 803C25D8 -> 803C25E0
- 803C25F8 -> 803C2600 -> 803C2608
- 803C261C -> 803C2624 -> 803C262C
- 801FFA04-801FFA24
- 800A7AF8-800A7AFC
- 803C01E8
- game/game_pacing.cpp:hle_HomeMenuStep
- tests/test_game_pacing.cpp

## 8017AC88 — функция 8017A7B0

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Stream-start wait: state 698 polls 80049000 (sound stream manager 8045D4F0) and gives up after 30 polls; the wait is for an asynchronous resource, so at 480 Hz the 1 s timeout shrank to 62 ms. SoundWaitStep makes the countdown consume nominal frames.

**Происхождение:** addic-extension-57.

**Поле:** `r31@block124+0x11D40`. Тип: `integer-step`; операции: `addic_`. Приоритет сканера: `medium`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_035.cpp:10652`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- 80049000 returns 1 while stream handle state (8004BA3C) is not 1 and 0 once ready (then applies f1 via 8004B4F8)
- 8017A7D4 dispatches state 698 to 8017AC68; AC5C sets +7488 (object+0x10000) to 30, AC70 polls, AC84..8C decrement and stay in 698 while >0
- Fix: hle_SoundWaitStep (map 80049000, retained body 80049004) adds 1-nominal_ticks before the caller's decrement for LR 8017AC74/8017AF5C only; other callers 80105ED0 and 801D4F34 have no counter
- tests/test_game_pacing.cpp: timeout lasts exactly 1 s at 30/60/120/240/480 Hz for both callers, pause keeps the counter, ready stream and other callers unchanged
- Earlier note: Branches for states 698/900 set +7488=30; AC68..88 and AF50..70 call 80049000 with fixed category/duration, decrement +7488 on its zero result, and transition state on underflow. Neighbor stores in this function (+5292/+5296/+7332) already incorporate elapsed time, but that does not settle these integer counters.

## 8017AF70 — функция 8017A7B0

**Статус:** `fixed` — Покрыто исправлением в исходниках.

**Что делает и почему такой вывод:** Same stream-start wait as 8017AC88 for state 834 (dispatch 8017A828 -> 8017AF50, stream id 2).

**Происхождение:** addic-extension-57.

**Поле:** `r31@block197+0x11D40`. Тип: `integer-step`; операции: `addic_`. Приоритет сканера: `inspect-loop`.

**Исходная привязка:** `generated/generation_6a8e49ea30acc1dd/game_035.cpp:10855`. Номер строки относится к сохранённой генерации; устойчивый ключ — адрес инструкции.

**Доказательства / связанные адреса:**

- 8017AF48 sets +7488 to 30, AF58 polls 80049000(2), AF6C..74 decrement; fixed by SoundWaitStep with LR 8017AF5C
- Earlier note: Branches for states 698/900 set +7488=30; AC68..88 and AF50..70 call 80049000 with fixed category/duration, decrement +7488 on its zero result, and transition state on underflow. Neighbor stores in this function (+5292/+5296/+7332) already incorporate elapsed time, but that does not settle these integer counters.
