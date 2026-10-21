# Offline timing audit

## Продолжение — 2026-10-21, вечер

Все кандидаты классифицированы: 3 165 не кадровые, 359 уже учитывают время, 54 исправлены, **0 открытых**. Исправлено ожидание звукового потока (`SoundWaitStep`, 8017AC88/8017AF70) и кроссфейд слоёв анимации (`CrossfadeStep`, 80165610), из-за которого обезьяны при высоком FPS почти не двигались. Кроссфейд — двойной учёт времени через вызовы функций (скорость проигрывания уже «кадры за вызов» × секунды); локальный сканер такой класс ошибок не находит. Сборка 135/135 (`generation_2a48cf24324130e2`) установлена и сверена по SHA-256. Подробности и отклонённые гипотезы — [timing-handoff.md](timing-handoff.md). Разделы ниже — более ранние снимки.

## Обновление сборки и установки — 2026-10-21 16:37:21 МСК

Актуальные исходники, включая SoundRetryStep, прошли полный build: **135/135 тестов**, 14.58 с. Генерация: `generation_bb9b4e9eb8c69408`. Сборка установлена в `S:/Games/Umbrella Chronicles`; все **102** файла install manifest совпали с dist по SHA-256. [Состав и контрольные суммы](timing-latest-deployment.json). Игра после установки не запускалась.

Это обновление заменяет прежние замечания ниже о невыполненной полной сборке и отсутствии установки. Классификация кандидатов не менялась: четыре места остаются открытыми. Повторное сканирование новой генерации ещё не выполнялось.


## Latest saved checkpoint, 2026-10-21

See [the continuation handoff](timing-handoff.md), [all 3,578 candidates](timing-candidates-all.md), and [52 fixed store operations plus cross-call fixes](timing-candidates-fixed.md). Matching JSON exports preserve per-candidate evidence and source hashes. The original 3,513-store baseline is closed; the extended scan has **four unresolved candidates**.

The last full build passed 135/135 tests **before SoundRetryStep was added**. Latest SoundRetryStep focused tests and 16/16 scanner tests passed; regenerate and run the full build before installing current source. The installed game was unchanged during this pass. Later sections retain chronological results; this checkpoint supersedes their earlier final-build/coverage wording.

Run from the repository root, after a normal translation/build:

```powershell
python -I tools/timing_audit.py
```

This reads the active generation from `generated/game_sources.cmake`, scans every
numeric `game_*.cpp` shard, and writes `build/timing_audit/report.json` and
`build/timing_audit/report.md`. It does not run the game, translate again, change
guest code, alter saves, or rebuild/install the executable. Python standard library
only; opcode operand roles come from the existing `tools/leaf_hybrid.py`.

For the current bat/shared-flight and monkey regions:

```powershell
python -I tools/timing_audit.py --focus 801C0000:801DFFFF --focus 801FFE34:80204FFF
```

This still scans everything and additionally writes `focus.md`. These are inclusive
function-entry address ranges, not proven class boundaries: shared helpers outside
the ranges remain in the full report. `--generation <directory>` selects another
generation; `--output <directory>` selects the report folder. Each run replaces its
report files. A previous `focus.md` is only refreshed when `--focus` is supplied.

## Reading the findings

The scanner tracks loads, pointer copies/constant offsets, arithmetic, and stores
within straight-line instruction blocks. A candidate requires a stored value to
depend on a previous load from the same non-stack address, with arithmetic in
between. Thus ordinary initializations, stack spills and register-only loop counters
do not become findings. Integer increments, float integration/damping and paired
single arithmetic are covered.

| Priority | Evidence | What still needs checking |
| --- | --- | --- |
| `high` | Float recurrence without recognized dt | Per-frame update, event response, or ordinary matrix math? |
| `medium` | Integer recurrence without recognized dt | Frame timer, event counter, reference count, or state transition? |
| `inspect-loop` | Recurrence lies in a backward-branch address interval | Actual loop and its invocation rate; address interval alone does not prove a cycle |
| `review-time-use` | Clock dt reaches this expression | Units, double scaling, branch coverage and timestep-independent thresholds |

Recognized direct clock fields are `0x804D3318 + 4/+8`. The target's small-data
bases are `r2=0x806CC700`, `r13=0x806CA620`. These are game-specific assumptions.
Clock presence elsewhere in the function never clears a candidate.

Each finding includes the guest store address, field offset relative to a symbolic
base, arithmetic operations, evidence instruction addresses, source path/line,
resolved direct callers, outgoing calls, and indirect-call count. JSON also contains
functions without findings, unsupported-operation counts and hashes of the scanned
source, dispatch table, analyzer and operand-role definitions. Markdown prioritizes
the candidates for reading. A register base such as `r31@block20` is local to that
function/block; it is not an identified actor type or runtime address.

HLE mappings come from the scanned generation's `game_table.cpp`. Known timing hooks
and retained original bodies are annotated, including conditional coverage. They
never suppress findings: `WorldRaycast` only changes one landing callsite, and
`FlightPathStep` only scales its distance argument in selected modes. Keep the small
`HOOKS`/`BODY_HOOKS` documentation table in sync when changing `game/game_pacing.cpp`.

## Coverage limits

This is a complete pass over the selected translated shards, not a proof that every
FPS dependency has been found. Branches/calls reset local dataflow; unmodeled
instructions invalidate their written registers, or the block when operand roles
are unknown. In particular, dt carried across calls or branches becomes unknown.
The JSON exposes those coverage gaps rather than declaring the function safe.

The scanner does not resolve virtual calls, indexed memory, cross-function
dataflow, arbitrary pointer aliases or host C++. Native HLE bodies absent from the
generation need separate review; their dispatch entries are inventoried. Local
loads/stores through vector helpers and model setters can hide position integration.
Collision reach thresholds, constant solver-step arguments, animation selection,
and incorrect powers of dt can fail without producing a recurrence candidate.

Do not automatically scale every finding or gate every function at 30 Hz. State
transitions and event counts must retain their meaning; already timed branches
must not be scaled twice. A fixed update rate for the whole simulation would also
need separate rendering/interpolation and an audit of input/event handling.

## First triage, 2026-10-21

The initial pass over `generation_a63c6e2b20561635` took approximately six seconds:
15,838 functions and 957,088 guest instructions. These counts describe scanned
code, not confirmed bugs. Regenerate the report for current candidate counts.

Calibration against previously diagnosed problems:

- `8014D36C`, store `8014D514`, object offset `+7752`: HUD per-call counter is found;
  its existing `HudNameDelay` hook is shown separately.
- `8021027C`, stores `802103B4`, `802103FC`, `80210444` and related stores: repeated
  spring damping is found, along with counter `802102FC`. The backward-branch
  heuristic places these in `inspect-loop`; this is not a reason to dismiss them.
- `801C44BC`, store `801C478C`: path recurrence remains visible despite the
  `FlightPathStep` hook. The caller's input scaling needs checking separately.

Targets for the reported remaining enemy problems, found without a new game run:

1. **Bat/shared flight collision and falling path.** `801C4158` multiplies fields
   `+8140`, `+8136`, `+8144` at `801C42F0`, `801C4358`, `801C4378` following collision
   tests. These may be collision response, so per-call multiplication alone does
   not prove a bug. Its caller `801C3794` handles state `+8174 == 61/62` and calls it
   at `801C37D0`. On the continuing-motion branch, `801C3878` adds the vector at
   `actor+8136` directly to model position, then writes the model transform at
   `801C389C`. Later, `801C38DC..801C38F0` updates that vector using clock dt.
   Confirmed and corrected: `VectorAddStep` scales only the position addition
   with return address `801C387C`; the gravity addition at `801C38F0` retains dt.
   The adjacent state handler `801C35BC` advances death/fall states 61/62 to
   grounded state 63 and removal state 100. Neither `FlightStep` nor
   `FlightPathStep` is called by this body.
   The position addition itself crosses helpers and is outside the local scanner's
   automatic recurrence detection; the collision findings led to it through callers.
2. **Monkey locomotion state.** `80201BD0`, store `80201ED4`, is already timed:
   `80201E58..1E5C` sets saved GPR31 to clock `804D3318`; `80201EC4` loads dt
   into f2, and `80201ED0` computes old value + constant * dt. A call boundary
   hid that pointer from the local analyzer. Its state-60 branch also loads dt
   at `80201DE0` for earlier motion calculations. Adjacent `+8568` increments at
   `80201EFC`/`80201F34` are state transitions gated by script calls; they should
   not be blindly turned into frame timers.
3. **Monkey jump interpolation.** `802007AC` advances elapsed field `+8564` by dt.
   `80200A80` normalizes it and calls smootherstep `80123D14` at `80200B88`.
   The result multiplies **target minus current world position**, not a fixed
   jump origin (`80200BB4..BDC`); this residual is added through the local model
   transform. Repeating the same alpha more often accelerates convergence.
   `JumpBlendStep` now uses `1 - (1-alpha)^step_scale`, only for return address
   `80200B8C`. Other smootherstep callers retain the original result. The original
   30 Hz result is preserved; changing alpha within an interval still has ordinary
   timestep integration error, so this is not bit-identical full-trajectory replay.
4. **Bat flight phases.** `801C56F0`, states 32/34, increments `+8208` by speed
   `+8176` at `801C57B8`, without dt. `FlightPhaseStep` temporarily scales that
   speed for this body and restores its exact bits on return or exception.
   States 31/33 and timer `+8672` already use dt and retain their original logic.

The audit script remains read-only. Gameplay corrections live in
`game/game_pacing.cpp`, with bindings in `project/hle_map.txt` and retained AOT
entries in `project/uc.cfg`.

Additional enemy triage: `801D1900` speed multipliers follow transitions out of
states 21/24; they are one-time setup, not repeated damping. `801CD674` and
`801CE428` multiply by -1 (direction reversal). `801CB86C`/`801CBAE0` add 2 to
a vector's Y coordinate after replacing that vector from matrix data. Monkey
`+2252` is a waypoint index into path `+2216` with stride 32, not a frame timer.
The `+932` candidates in `80200048` and `802034AC` are damage-response paths,
including event type checks and virtual damage slot +844; no per-frame fix is
justified there. No separate monkey death-fall timing bug was confirmed.

The wider investigation ledger is `build/timing_audit/remaining-triage.md`, with
every out-of-focus high candidate in `remaining-triage-inventory.md`. Unresolved
entries are retained for follow-up; they are not certified safe. In particular,
`802F03F0` uses alpha 0.06, but its live invocation cadence is unproven;
`802F13E8` needs the producer/units of field +152. Raw constants alone do not
establish a gameplay bug.

Validation on 2026-10-21: `test_game_pacing` covers 30/60/120/240/480 Hz, pause,
unrelated callsites, velocity preservation and exception restoration. An offline
probe (`build/enemy_probe.cpp`) also ran the actual retained AOT phase body and
smootherstep tail using constants from an existing RAM capture and synthetic
model getters/setters: both flight states transitioned after 0.8 seconds at every
tested rate; blend composition passed. This does not exercise animation, collision
or a complete monkey jump. No new game session was needed for these checks.

## Completed baseline investigation, 2026-10-21

`docs/testing/timing-triage.json` tracks every one of the 3,513 original store
PCs, including candidates removed from generated code by native replacements.
Its `complete` flag covers this recorded baseline. Counts describe
store instructions, not independent bugs. Evidence records producer/caller PCs;
an elapsed-time read elsewhere in a function is insufficient to close a finding.

Final classification: **3,111 not frame-based, 359 already timed, 43 covered by
confirmed fixes, zero unresolved**. These add up to all 3,513 baseline stores;
the 43 include fixes from earlier work. Every baseline store has a classification,
reason and evidence; removed/replaced stores retain their entries. Current scanner
output is regenerated separately in `build/timing_audit_current/report.json`.
Clearing this baseline does not certify code outside the scanner's coverage.
Before the addic/addic_ scanner extension, generation `6a8e49ea30acc1dd` reported
3,507 candidates: six baseline stores moved into native replacements. The extended
scanner then found 57 additional stores (3,564 in the current-generation report);
the durable union also retains removed baseline stores and manual discoveries.

Additional confirmed corrections in `game/game_pacing.cpp`:

- ODE auto-disable quiet-step countdown (`800F1D08`) advances in nominal solver
  substeps; the separate seconds countdown retains dt. Fractional state is reset
  on motion and removed on body destruction. An offline original-AOT comparison
  matched all 352 body bytes over 9,600 nominal-rate cases.
- A fresh paused prop no longer receives its initial pending integration step.
- Camera history (`801B0AAC`) samples on nominal-frame boundaries and interpolates
  output between samples, preserving the selected delay at higher update rates.
- Sole-reader coefficients normalize lighting retention (`80140C4C`), projected
  UV drift (`80158C08`), texture scrolling (`80189034`, `801897B8`, `80189814`),
  model-history sampling (`801F2020`), and the slow-motion ramp (`800667B0`).
  Both slope and intercept of the ten-layer texture speeds are scaled.
- Model history in `801926B0` divided `(10*dt)/(20*dt)`, cancelling elapsed time.
  The callsite-specific matrix-identity wrapper scales its numerator once per
  child, keeping other matrix calls unchanged.
- Nearby-actor action delays (`801A5130`, field +8488) count nominal ticks;
  the rest of each actor update continues at the current update rate.
- Rumble patterns (`80136EEC`) consume one byte per nominal tick instead of one
  byte per update; original list removal and motor transitions remain in AOT.
- Trail history (`80162A68`) samples at nominal-frame cadence; projection still
  runs every update. Weapon history (`8006DB34`) does the same for its eight
  stored samples, while fade continues to use elapsed time. Catch-up repeats
  the current input sample; low-FPS shape interpolation remains a limitation.
- UI particle gravity (`802448B4`) and angular drag (`802448F8`, `80244928`)
  use the nominal step scale. The configured drag is restored after the call,
  including exceptions. The two angular stores were absent from the baseline.
- Script timeline (`80211734`) advances its clock by elapsed nominal frames
  before evaluating event thresholds; the original default event scale remains 1.
- Effect retirement (`801B916C`) preserves the nominal-frame grace period before
  checking animation completion. Only unchanged record counters are adjusted;
  actor scanning, completed-record removal and newly replaced records retain
  their original behavior.
- Target-list refresh (`801297BC`) counts nominal frames at its specific helper
  callsite; target scanning and state callbacks still run every update.
- Voice envelopes and stop delays (`8039C1D0`) run through the complete locked
  sound-manager frame callback (`803AA7B0`) at nominal cadence. The five affected
  counters were absent from the baseline and are recorded as supplemental findings.
  Running the full manager walk retains list removal and priority sorting; each
  catch-up iteration obtains the current voice list. AX buffer work keeps its
  independent three-millisecond clock.
- Sound-start retries (`8004A6D8`) retain their six-nominal-frame window. The
  callsite-specific `SoundRetryStep` compensates the caller's decrement before
  retrying, leaves initial requests intact, and preserves pending retries during
  pause. A resource becoming available after 100 ms is covered at 30–480 Hz.
- Home menu (`803C1BF8`) timeouts, fades and controller updates use nominal
  cadence. Button press/release edges are latched across skipped updates, consumed
  once during catch-up, and cleared on initialization or controller replacement.
  Caller-owned KPAD words are restored on return or exception. Initialization
  executes once even when no tick is due; catch-up stops at menu completion.

Focused regressions cover 30/60/120/240/480 Hz, pause, multiple objects and
catch-up where applicable. Home-menu regressions additionally cover short clicks,
disconnect/reconnect, initialization, completion and input restoration. The
retirement, target-refresh and Home-menu regressions failed with original cadence
and passed after correction. The sound-manager test checks wrapper cadence and ABI;
it does not execute a real audio voice or validate audible output.
These are offline checks; new game sessions and installation into the game
directory have not been performed during this investigation. Camera, timeline,
trail and retirement regressions use controlled original-body fixtures; they do
not establish whole-game visual equivalence.

Last full build, before SoundRetryStep: `build/timing-complete-build.log`,
**135/135 CTest tests passed** in 14.36 seconds; `dist` staged with `--no-install`.
Latest focused pacing tests pass after SoundRetryStep. The scanner's 16 Python
tests pass after addic/addic_ support. A new full build remains required.

Important exclusions established during the final pass:

- Video counters are driven by callback `802FCCE0`, registered in the kind-2
  callback list. `80343948` waits on VI retrace before dispatching that list through
  `8034078C`; native VI retains its PAL/NTSC clock independently of render FPS.
- Audio buffer/fade counters in `80397BB8` and `8039CF68` originate in the AX
  callback/message thread, whose native timer period is three milliseconds.
- Selection index `80256B04` and Wii Remote volume levels are input-event counts.
  Their upstream dispatch uses pressed-button words, not continuous frame time.
- Animated-model context time flows through the embedded callback at model+216,
  bound back to its model by `80031B08`/`80031EFC`. Getter `80032AA0` supplies
  elapsed playback and `80031344` divides by the nominal rate. The separate plain
  node getter must not be used as evidence that this context has a fixed timestep.

The historical `UiAnimUpdate` label for `8039C1D0` was misleading: this is sound
processing. The new sound hook wraps the manager entry and retains its AOT tail;
it does not restore the removed callback-skipping implementation.

Remaining validation limits: no exhaustive scene-by-scene playthrough, no physical
Wii Remote rumble check, and no live audio/Home-menu check in this pass. Camera and
trail shape at low FPS, time-varying monkey interpolation and complete collision
trajectories still require visual confirmation. Scanner gaps listed above remain.

## Scanner tests

```powershell
python -I tests/test_timing_audit.py .
ctest --test-dir build -R test_timing_audit --output-on-failure
```

The CTest entry becomes available after CMake regeneration. Tests cover positive
recurrences, dt separation, paired singles, pointer changes, control-flow/call
barriers, stack aliases, PPC's zero-base immediate semantics, active-generation
selection, and CLI report/HLE provenance.
