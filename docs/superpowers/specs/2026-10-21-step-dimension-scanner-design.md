# Step-dimension scanner and automatic pacing fixes — design

Date: 2026-10-21. Owner request: find every place where game logic depends on the update rate
without long live game sessions; fix most of them automatically; report only what is doubtful or
cannot be fixed safely. **Any risk of breaking the game must be excluded.**

## Why

The Wii game assumes one update per nominal frame (30 Hz, sometimes 60 Hz in menus). The port runs
updates at the render rate (up to 480 Hz) and supplies the real timestep. Bugs found so far were
located one at a time with live runs (hours each). The existing scanner (`tools/timing_audit.py`)
only sees per-call recurrences inside one function and missed the most damaging recent bug:
the animation crossfade `80165610` multiplied two update-rate-dependent factors that met through a
call chain and an object field (fixed by `CrossfadeStep`). Remaining bugs of that kind are
invisible to the current tooling.

## Goals

1. Interprocedural analysis of the translated game that assigns every value its dependence on the
   update rate, and flags code whose behaviour per nominal frame changes with the update rate.
2. Automatic fixes for patterns with a proven fix template, applied without editing generated code.
3. A report (Markdown + JSON) of everything doubtful, with evidence, for manual review.
4. Each automatic fix is proven offline on the real translated code before it can enter a build.

## Non-goals

- Fixing renderer, audio-thread (AX, 3 ms timer) or host C++ timing.
- Interpolation or a fixed-rate simulation redesign.
- Replacing the existing hand-written hooks in `game/game_pacing.cpp`; the tool must recognise them
  as already covered.
- Proving the absence of all bugs.

## Anti-goals (failure even if the tool "works")

- Any change of behaviour at the 30 Hz original mode (scale = 1). This must be impossible by
  construction and is also tested.
- An automatic fix to an event counter, a state transition count, a reference count, input handling
  or code that already uses elapsed time correctly (double fixing).
- Automatic fixes applied without the offline equivalence proof passing.
- A report so noisy that review is again thousands of entries.

## Model: step exponent

Let `s` = step scale (actual update duration / nominal frame duration; 1 at 30 Hz, 1/16 at 480 Hz;
the runtime value is `uc::game_step_scale()`). Each value carries an exponent `k` meaning
"value ∝ s^k when the update rate changes":

| Source | k |
|---|---|
| constants, per-call constants, game state read back unchanged | 0 |
| clock `804D3318+4` / `+8` (dt seconds) | 1 |
| clock rate/target (`+28`, `+40`) → frames per second | 0 (constant) |
| `801AEFEC` (dt·fps, nominal frames this update), `801AF03C`, `ActorStepTime` `800C790C` | 1 |
| fields written from a k=1 value (e.g. animation playback `this+16`) | 1 |
| unknown | ⊥ (unknown) |

Arithmetic: `a*b` → k_a + k_b, `a/b` → k_a − k_b, `a±b` → equal k required (mixed → ⊥ and noted),
comparisons record both sides. Calls use function summaries (return k as a function of argument
k), computed to a fixpoint. Object fields are keyed by (class, offset): class from the vtable
written in constructors or found in RAM dumps (`build/autotest/*/dump_*.bin`); fields without a
class key fall back to (function, base register) and are marked lower confidence. Virtual call
targets are resolved from vtables found in the dumps and in the DOL data; unresolved virtual calls
make the result ⊥, never "safe".

HLE/native functions absent from generated code get hand-written summaries in a small table
(like today's `HOOKS`/`BODY_HOOKS` table in `timing_audit.py`, which must be kept in sync).

## What is flagged

Correct per-update change of persistent state has k = 1 (so that the change per nominal frame is
rate independent). Patterns:

| Pattern | Rule | Example (known) |
|---|---|---|
| P1 per-call accumulation | `x = x + d`, d has k = 0 | flight phase `801C56F0`, prop springs |
| P2 double time factor | `x = x + d`, d has k = 2 | crossfade `80165610` |
| P3 per-call smoothing | `x = x + a·(t − x)` or `x·c`, a/c with k = 0 | jump blend `80200B88`, lighting `80140C4C` |
| P4 threshold vs step | compare k = 1 quantity with k = 0 threshold | monkey landing ray `80200DFC` |
| P5 per-call countdown | integer `n = n − 1` per update gating an action | sound wait `8017A7B0`, target refresh |

Persistent state = object fields and globals (not stack). Already-covered sites (existing hooks,
retained bodies) are reported as covered, not refixed.

## Automatic fixes

Fixes are records in a new data file, e.g. `project/pacing_fixes.txt`, consumed by one generic
native mechanism; generated code is never edited. Each record names a call site (function entry +
caller LR, as all current hooks do) and a template:

| Template | Effect | Used for |
|---|---|---|
| scale-arg / scale-ret | multiply or divide one FPR argument/return by `s` (or `s^n`) at that LR | P1 (×s), P2 (÷s) |
| pow-alpha | replace blend factor a by `1 − (1 − a)^s` at that LR | P3 |
| nominal-tick countdown | counter consumes nominal ticks instead of one per call | P5 |

P4 and anything else is report-only unless a template is added deliberately later. Only sites whose
whole chain (source of time → arithmetic → persistent store) is resolved without ⊥ and matches one
template exactly are candidates for an automatic fix.

### Safety gates (all must pass, otherwise the site goes to the report)

1. **Identity at 30 Hz.** Every template is the identity when `s == 1` (checked by construction and
   by a generated unit test per record). Pause (`s == 0`) behaviour follows today's hooks (no
   division by zero, state untouched).
2. **Offline equivalence proof on real code.** A probe executable linked against the translated game
   (as `build/enemy_probe.cpp` does) loads a RAM dump state, runs the original function once with a
   30 Hz step, and runs the fixed function N times with step 1/N (N = 2, 4, 16), then compares the
   touched persistent state within a stated tolerance. A site without a usable dump state or with a
   failing comparison is not fixed.
3. **No overlap.** A site already handled by a hand-written hook, or whose function contains another
   flagged or ⊥ path into the same field, is not auto-fixed.
4. **Full build** (`build.bat --no-install`, all tests) passes before anything is installed.
5. At most one short live A/B (30 vs 480 Hz) at the end for the riskiest class, as the owner prefers
   few runs.

## Validation of the tool itself

The tool is not trusted until, on the current generation, it:
- finds the known bugs (crossfade `80165610`, landing ray `80200DFC`, flight phase `801C56F0`,
  prop springs `8021027C`, sound wait `8017A7B0`, sound retry `8004A6D8`, HUD name delay `8014D36C`),
  and reports them as covered by the existing hooks;
- with the existing hooks virtually removed, proposes the same fix template the hand-written hook
  implements (where a template exists) and its equivalence proof passes;
- does not flag the known non-bugs (selection index `80256B04`, video counters, AX buffer counters).

## Output

- `build/step_audit/report.json` + `report.md`: every flagged site, pattern, k chain with addresses,
  confidence, fix status (auto-fixed / covered / report-only and why).
- `project/pacing_fixes.txt` entries for proven fixes, each with a comment line naming the evidence.
- Durable summary appended to `docs/testing/` like the current registries.

## Constraints

- Python standard library only for the analysis (like `timing_audit.py`); the probe is C++ built
  with the existing toolchain.
- Read-only towards the game and saves; probes never run the game; harness rules apply to any
  final live run (1280x720, copied saves, fps-limit 0).
- Reuse `tools/leaf_hybrid.py` operand roles and the generation selection of `timing_audit.py`.

## Left to the builder

- Exact data file format and the native mechanism's shape (one dispatcher per hooked entry is fine).
- Tolerances for the equivalence proof per template (state them in the report).
- How far field keys need class precision before precision stops paying off.

## Owner decisions after the builder check

- **Strict proof (owner).** An automatic fix requires exact composition: the fixed function run N times
  at step 1/N must reproduce the original single 30 Hz step within float rounding only
  (relative 1e-5 per touched value). This must hold for N = 2, 4, 16 and for the update rates of
  real displays, 144, 165 and 240 Hz (scales 5/24, 2/11, 1/8 of the 30 Hz nominal frame). Sites with feedback/integration error
  go to the report. The report is expected to be the main product; few automatic fixes is acceptable.
- **No new capture runs (owner).** Only the existing dumps in `build/autotest/*/dump_*.bin` (chapter 0:
  train, monkey courtyard, bat boss) are used. Sites without a usable dump state go to the report as
  "unproven: no state"; they are reviewed manually.

## Decisions after the builder check

- **Fix point.** The record goes on the call boundary nearest to the persistent store where the k-chain crosses as an FPR argument or return. A chain with no such boundary is report-only. P5 uses an entry hook on the owning function that compensates the caller's decrement with nominal ticks, as `TargetRefreshStep` and `SoundWaitStep` do; it shares the dispatcher.
- **Only function-entry/LR hooks are in scope.** A store inside a loop body with no call boundary is report-only. The dispatcher sits only on hooked entries and compares the LR against a small per-entry table. Performance: no regression in the deterministic demo bench (`uctest bench --demo`, AGENTS.md rule).
- **Default for unknown provenance.** A field gets k = 0 only if every store to it found in the generation writes a k = 0 value. Otherwise, and if there are no stores at all, it is ⊥. Target: the report is reviewable, aim for under ~200 entries; if it is larger, tighten the rules before reviewing.
- **Hand-written hooks that replace a body** (for example `FlightPathStep`, `HomeMenuStep`) count as "found and covered" in the validation, with no template required. The template must match only where the hand-written fix is a plain arg/ret scale, a blend power or a nominal-tick countdown (crossfade, flight phase, jump blend, sound wait, target refresh).
- **Shared tables.** `HOOKS`/`BODY_HOOKS` move into a module both tools import.
- **Code coverage.** The scanner covers what the active generation contains; if the game loads REL modules outside it, that is reported as a coverage gap.

## Calls made by me (not discussed with the owner)

- New tool next to `timing_audit.py` rather than extending it in place.
- Fix records in a data file instead of generated C++ hooks.
- Automatic fixes limited to the four templates above; P4 report-only.
