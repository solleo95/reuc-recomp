# Step-Dimension Scanner Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Find every update-rate-dependent site in the translated game offline, fix the ones whose fix is proven exact on real code, and report the rest.

**Architecture:** A Python package `tools/step_audit/` parses the active generation, resolves virtual calls from RAM dumps, and runs an interprocedural abstract interpretation that tags each value with its step exponent k. Persistent stores are matched against patterns P1–P5. Fix candidates become records in `game/pacing_fixes.inc`, expanded by macros into ordinary HLE hooks. A C++ probe `step_probe.exe` proves each record on dumped game state before it is kept.

**Tech Stack:** Python 3.10 standard library (`python -I`), existing C++20/clang-cl/CMake build, translated game library `uc_game.lib`.

**Spec:** `docs/superpowers/specs/2026-10-21-step-dimension-scanner-design.md`

## Global Constraints

- Analysis is Python standard library only, runs as `python -I`; reuses `tools/leaf_hybrid.py` (`OP`, `ROLES`) and the generation resolution of `tools/timing_audit.py`.
- Generated code (`generated/`) is never edited by hand or by the tool.
- Every fix template is the identity at `s == 1`; at `s == 0` (pause) it never divides by zero and leaves the call unchanged.
- Automatic fix only with exact composition: relative error ≤ 1e-5 per touched float, exact for integers, at update rates 60, 120, 480 Hz and 144, 165, 240 Hz (scales 1/2, 1/4, 1/16, 5/24, 2/11, 1/8 of the 30 Hz nominal frame).
- Only existing dumps `build/autotest/*/dump_*.bin` are used; no new capture runs. Sites without a usable state are "unproven: no state".
- Fix points are function entries with caller LR only. Sites without such a boundary are report-only.
- Existing hand-written hooks (`HOOKS`/`BODY_HOOKS`) are never refixed; they are reported as covered.
- Report target: under ~200 entries; if larger, tighten rules before review.
- Performance: no regression in `py -3.13 -m uctest bench <A> <B> --demo fight2`.
- Live runs: at most one A/B at the end, harness only (1280x720, copied saves, fps-limit 0). Never fullscreen.
- Commits only when the owner asks (user rule); tasks end with a checkpoint instead.

## Review Focus

- A k = 1 value that reaches a store through a conditional branch merge with a k = 0 path: expected result ⊥ (report), never an automatic fix. Test in Task 4.
- A field written from both a k = 1 and a k = 0 value in different functions: expected ⊥ for every reader of that field. Test in Task 5.
- A virtual call whose slot resolves to targets with different summaries: expected ⊥, never "safe". Test in Task 3.
- A probe function that calls a host-side HLE function (GX draw, sound, OS wait) or throws: expected "unproven: side effect", the record is not written. Test in Task 7.
- A fix record whose entry does not start with `stwu r1,-N(r1)`: expected report-only, since `call_body` cannot lower the prologue. Test in Task 6.

---

### Task 1: Shared hook tables

**Files:**
- Create: `tools/pacing_hooks.py`
- Modify: `tools/timing_audit.py:27-73` (remove the tables, import them)
- Test: `tests/test_pacing_hooks.py`

**Interfaces:**
- Produces: `HOOKS: dict[str, str]`, `BODY_HOOKS: dict[str, str]` (unchanged contents), `hle_map_hooks(path: Path) -> dict[str, str]` returning address → name for every `@ADDR Name` line whose name is in `HOOKS`.

- [ ] **Step 1: Write the failing test** `test_hle_map_and_tables_agree`: every hook name in `project/hle_map.txt` that appears in `game/game_pacing.h` as `hle_<Name>` is a key of `HOOKS`, and every `BODY_HOOKS` value is a key of `HOOKS`. Second test `test_timing_audit_uses_shared_tables`: `timing_audit.HOOKS is pacing_hooks.HOOKS`.
- [ ] **Step 2: Run** `python -I tests/test_pacing_hooks.py .` — expected FAIL (module missing).
- [ ] **Step 3: Move the tables** into `tools/pacing_hooks.py`, add `hle_map_hooks`, import in `timing_audit.py`.
- [ ] **Step 4: Run** `python -I tests/test_pacing_hooks.py .` and `python -I tests/test_timing_audit.py .` — expected PASS, 16/16 for the old scanner.
- [ ] **Step 5: Register** both Python tests in `tests/CMakeLists.txt` like `test_timing_audit`. Checkpoint.

### Task 2: Instruction IR

**Files:**
- Create: `tools/step_audit/__init__.py`, `tools/step_audit/ir.py`
- Test: `tests/test_step_audit.py` (one file for the package, grows per task)

**Interfaces:**
- Produces:
  - `@dataclass(frozen=True) class Insn: address: int; op: str; args: tuple; roles: tuple; line: int; call: str | None; indirect_slot: int | None; branch: tuple[int, ...]; returns: bool`
    - `call` is the callee address (8 hex) or `"hle:<Name>"`.
    - `indirect_slot` is the vtable byte offset of the preceding `lwz r12,N(r12)`.
    - `branch` lists `goto L_X` targets.
  - `@dataclass class Function: address: str; path: str; insns: list[Insn]; labels: set[int]; prologue_frame: int | None` — `prologue_frame` is N when the first instruction is `stwu 1,1,-N`.
  - `load_generation(directory: Path) -> dict[str, Function]`, using `timing_audit.resolve_generation`.

- [ ] **Step 1: Write failing tests** on a synthetic shard string:
  - `test_parses_calls_and_slots`: `fn_X(c)` gives `call == "X"`; `hle_Foo(c)` gives `"hle:Foo"`; the `lwz r12,12(r12)` then `call_indirect` pair gives `indirect_slot == 12`.
  - `test_prologue_frame`: `stwu(c, 1, 1, -256)` gives 256; a function without it gives None.
  - `test_branch_targets_and_labels`.
- [ ] **Step 2: Run** `python -I tests/test_step_audit.py .` — FAIL.
- [ ] **Step 3: Implement** with the `FUNCTION`/`INSTRUCTION` regexes and `OP`/`ROLES`; unparsed instructions become `Insn(op="?")`.
- [ ] **Step 4: Run** — PASS. Also `load_generation` on the active generation returns ≥ 15000 functions (assert in a test marked by env `UC_STEP_AUDIT_FULL=1`).

### Task 3: Classes and virtual targets from dumps

**Files:**
- Create: `tools/step_audit/classes.py`
- Test: `tests/test_step_audit.py`

**Interfaces:**
- Consumes: `Function`, `Insn` (Task 2).
- Produces:
  - `scan_dump(path: Path, functions: set[str]) -> DumpIndex`. A vtable is a 4-aligned word run of ≥ 2 entries that are function entries or 0. An object is any word equal to a vtable start, at object offset 0 or 16 (both layouts occur in this game).
  - `DumpIndex.vtables: dict[int, list[str|None]]`, `DumpIndex.objects: dict[int, list[int]]` (vtable → object addresses), `DumpIndex.slot_targets(slot: int) -> set[str]`.
  - `merge(indexes: list[DumpIndex]) -> DumpIndex`.

- [ ] **Step 1: Write failing tests** on a synthetic 64 KiB buffer: `test_finds_vtable_and_objects`, `test_slot_targets_union`.
- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement** with `struct.unpack` over MEM1 (24 MiB) and MEM2 (64 MiB at file offset 24 MiB, guest base 0x90000000). Cache the result per dump in `build/step_audit/cache/<sha1 of path+mtime>.json`.
- [ ] **Step 4: Run** — PASS. Full-mode assertion: in `build/autotest/episode0-480--2-1792593132995846900/dump_1.50.bin` (or any dump that still exists), vtable `803FA8EC` slot 12 resolves to `80165610`, and vtable `804094F4` slot 772 resolves to `800C790C`.

### Task 4: Step exponent lattice and intra-function analysis

**Files:**
- Create: `tools/step_audit/kdim.py`
- Test: `tests/test_step_audit.py`

**Interfaces:**
- Consumes: `Function` (Task 2).
- Produces:
  - `@dataclass(frozen=True) class K: k: int | None; sources: frozenset[str]; chain: tuple[int, ...]; fields: frozenset[tuple]` — `k=None` is ⊥, `fields` are (base key, offset) loaded on the way.
  - `analyze(function: Function, env: Env) -> FunctionResult`.
  - `FunctionResult.ret_k: dict[str, K]` for r3/f1 at return.
  - `FunctionResult.stores: list[Store]`, with `Store(address, key, value: K, prior_load_same_key: bool, op_kind)`.
  - `FunctionResult.compares: list[Compare]`.
  - `Env` supplies the callee summaries, field k values, argument k values and clock sources.
- Rules (from the spec): `a*b` → `k_a+k_b`; `a/b` → `k_a−k_b`; `±` with unequal k → ⊥; loads from `804D3318+4/+8` → k = 1; loads from `+28/+40` → 0; integer immediates → 0. Join at labels over a CFG worklist: equal k is kept, otherwise ⊥.

- [ ] **Step 1: Write failing tests** (synthetic functions):
  - `test_dt_times_frames_is_k2`: a load from `804D331C`, times the result of a callee summary with k = 1, stored to `r3+16` → stored k == 2.
  - `test_per_call_increment_is_k0`: `lfs; fadds const; stfs` on the same field → stored k == 0 with `prior_load_same_key`.
  - `test_branch_merge_mixed_is_bottom` (Review Focus 1).
  - `test_fmadds_operand_order`: `op_fmadds(c, t, a, b, c2)` computes `a*c2 + b`.
- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement** the forward worklist over basic blocks. Calls apply `Env.summary(callee, arg_ks)`. Clobbered volatile registers become ⊥ unless the summary provides them.
- [ ] **Step 4: Run** — PASS.

### Task 5: Whole-program fixpoint and patterns

**Files:**
- Create: `tools/step_audit/program.py`, `tools/step_audit/patterns.py`
- Test: `tests/test_step_audit.py`

**Interfaces:**
- Consumes: `analyze`, `K` (Task 4); `DumpIndex` (Task 3); `pacing_hooks` (Task 1).
- Produces:
  - `run_program(functions, dump_index, hle_summaries: dict[str, K]) -> Program`.
    - Field keys are (vtable, offset) when the base is a `this` whose class is known, else (function, base register).
    - A field is k = 0 only if every store found writes k = 0. A field with mixed or no stores is ⊥.
    - Iterate until no field or summary changes.
  - `find_patterns(program) -> list[Finding]`, with `Finding(pattern: str, function: str, store: int|None, field, chain, confidence: str, covered_by: str|None)`.
    - P1: k=0 increment with `prior_load_same_key`.
    - P2: k=2 increment.
    - P3: blend with a k=0 factor.
    - P4: compare k=1 vs k=0 constant.
    - P5: integer −1 per call gating a call or a state store.
  - `HLE_SUMMARIES` table in `patterns.py` for native functions absent from the generation (start with `800C790C` ActorStepTime → k=1, `80032AA0` → k=1, the leaf helpers in `project/hle_map.txt` → transparent).

- [ ] **Step 1: Write failing tests:** `test_mixed_field_is_bottom` (Review Focus 2), `test_covered_site_reports_hook_name`, `test_non_bug_selection_index_not_flagged` (a synthetic replica of the `80256B04` input-edge increment, gated by a pressed-button load, must not be P1).
- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement.** Never-flag rule: a P1/P5 increment whose block is reachable only through a branch on a value loaded from an input or event word (`ui`, KPAD pressed words) is classified as an event, not a frame.
- [ ] **Step 4: Run** — PASS.

### Task 6: Fix proposer and fix record format

**Files:**
- Create: `tools/step_audit/fixes.py`, `game/pacing_fixes.inc` (empty table), `game/pacing_fixes.cpp`
- Modify: `game/CMakeLists.txt` (add `pacing_fixes.cpp` to `uc_game_pacing`), `game/game_pacing.h` (declare the macro-generated hooks via the same `.inc`)
- Test: `tests/test_step_audit.py`, `tests/test_pacing_fixes.cpp` (+ `tests/CMakeLists.txt`)

**Interfaces:**
- Consumes: `Finding` (Task 5), `Function.prologue_frame` (Task 2).
- Produces:
  - `propose(findings, functions) -> list[FixRecord]`, with `FixRecord(entry: str, frame: int, lr: str, template: str, operand: str, power: int, evidence: str)`.
    - Templates: `scale` (multiply FPR `operand` by s^power before the body, power may be negative), `scale_ret` (after the body), `pow_alpha` (f1 = 1−(1−f1)^s after the body), `tick_countdown` (counter at `rN+off` gets `+1−nominal_ticks`; `operand` = `"r31+0x11D40"`).
    - Only for fully resolved, uncovered, feedback-free chains. The nearest call boundary to the store where the chain crosses as an argument/return.
  - `write_inc(records, path)`: one line per record, `PACING_FIX(0x80165610, 256, 0x8016975C, scale, 1, -1)  // evidence`.
  - `sync_bindings(records, hle_map: Path, uc_cfg: Path)`: rewrites only the block between `# BEGIN step_audit` and `# END step_audit` in both files: `@ENTRY PacingFix_ENTRY` and `entry ENTRY+4`.
  - C++: the macro `PACING_FIX(entry, frame, lr, template, reg, power)` defines `void hle_PacingFix_<entry>(ppc::Ctx&)`. One function per entry; several records per entry are an LR switch inside it. It uses `call_body` (move `call_body` from `game_pacing.cpp` into an internal header `game/pacing_body.h`).

- [ ] **Step 1: Write failing tests:**
  - Python: `test_no_prologue_is_report_only` (Review Focus 5), `test_covered_site_gets_no_record`, `test_sync_bindings_touches_only_block`.
  - C++ `test_pacing_fixes`:
    - for every record in a test-local table using each template: at s = 1 the body sees identical registers and memory;
    - at s = 0 there is no division and the body is called unchanged;
    - with a non-matching LR the call is unchanged;
    - at s = 1/16 the `scale` operand equals operand·s^power.
- [ ] **Step 2: Run** `python -I tests/test_step_audit.py .` and `ctest --test-dir build -R test_pacing_fixes` — FAIL.
- [ ] **Step 3: Implement** `fixes.py`, the macro in `game/pacing_fixes.cpp` and `pacing_body.h`.
- [ ] **Step 4: Run** both — PASS. Run `build.bat --no-install` — 136+/136+ tests pass and behaviour is unchanged (empty table).

### Task 7: Equivalence probe

**Files:**
- Create: `tools/step_probe/step_probe.cpp`
- Modify: `tools/CMakeLists.txt` (or the top-level CMake) to build `step_probe.exe`, linking like `build/enemy_probe_build.bat`
- Create: `tools/step_audit/probe.py`
- Test: `tests/test_step_audit.py` (Python side, with a fake probe), plus a CTest `test_step_probe_selfcheck`

**Interfaces:**
- Consumes: `FixRecord` (Task 6), `DumpIndex.objects` (Task 3).
- Produces:
  - `step_probe.exe <job.json>` with job `{dump, entry, lr, template, operand, power, this, args:{r4:..,f1:..}, cases:[[p,q],...]}`. It prints one JSON line `{"status":"pass"|"fail"|"unproven","reason":...,"max_rel":...}`.
    - Case `[p, q]`: run the original body p times at a nominal step from the same snapshot, versus the record-wrapped entry q times at step p/q nominal.
    - Compare every changed MEM1/MEM2 word: floats rel ≤ 1e-5, other words exact.
    - Any `ppc::trap`, exception, or call into a host-side HLE outside an allowlist (math/leaf helpers) → `unproven`.
  - `probe.prove(record, dump_index, functions) -> ProofResult`.
    - Picks `this` from objects of the class owning the field.
    - Args: the scaled FPR operand gets the nominal value (seconds `2/59.94` or frames `1.0`, from the k source).
    - Any other argument register read before written in the entry → `unproven: needs context`.
- Cases (scale = 30 Hz nominal / update rate): `[[1,2],[1,4],[1,16],[5,24],[2,11],[1,8]]` — 60, 120, 480, 144, 165 and 240 Hz.

- [ ] **Step 1: Write failing tests:**
  - `test_step_probe_selfcheck`, a C++ test using a synthetic body registered in the dispatch table: x += f1·k passes for `scale`, x = x·0.9 per call fails for `scale`, a body calling `hle_GXBegin` is `unproven` (Review Focus 4).
  - Python `test_prove_needs_context`.
- [ ] **Step 2: Run** — FAIL.
- [ ] **Step 3: Implement.** The probe sets the step scale through the real `hle_GameFrameWait`, as `enemy_probe.cpp` does (clock tail replaced). It snapshots memory with a copy of both RAM views.
- [ ] **Step 4: Run** — PASS. Real-code check: the record `PACING_FIX(0x80165610, 256, 0x8016975C, scale, 1, -1)` proves `pass` on a dump containing the monkey anim object `0x80FEA050` (or any object of vtable `803FA8EC`), with `CrossfadeStep` temporarily excluded from the probe's dispatch table.

### Task 8: CLI, report and validation on the real generation

**Files:**
- Create: `tools/step_audit/__main__.py`, `tools/step_audit/report.py`
- Test: `tests/test_step_audit_known.py` (runs only with `UC_STEP_AUDIT_FULL=1`; registered in CTest with that env and label `slow`)

**Interfaces:**
- Consumes: everything above.
- Produces: `python -I -m tools.step_audit [--generation DIR] [--output build/step_audit] [--dumps GLOB] [--ignore-hooks] [--prove] [--apply]`.
  - Writes `report.json` and `report.md`: each finding with pattern, chain addresses, confidence, and fix status (`auto-fixed` / `covered:<Hook>` / `report: <reason>`).
  - `--apply` writes `game/pacing_fixes.inc` and the binding blocks only for `pass` records.

- [ ] **Step 1: Write the failing test** `test_known_sites` (spec "Validation of the tool itself"):
  - normal run: findings exist and are `covered:<Hook>` for `80165610` (CrossfadeStep), `80200DFC` (WorldRaycast), `801C56F0` (FlightPhaseStep), `8021027C` (PropStep), `8017A7B0` (SoundWaitStep), `8004A6D8` (SoundRetryStep), `8014D36C` (HudNameDelay);
  - `--ignore-hooks`: proposed templates are crossfade `scale` power −1 at LR `8016975C`, jump blend `pow_alpha` at `80123D14` LR `80200B8C`, sound wait `tick_countdown` at `80049000` LRs `8017AC74`/`8017AF5C`, target refresh `tick_countdown` at `8012AEE8` LR `801297B0`; body-replacement hooks (`FlightPathStep`, `HomeMenuStep`, `PropStep`) are found with no template required;
  - not flagged: `80256B04`, video callback counters under `802FCCE0`, AX counters in `80397BB8`/`8039CF68`.
- [ ] **Step 2: Run** `$env:UC_STEP_AUDIT_FULL=1; python -I tests/test_step_audit_known.py .` — FAIL.
- [ ] **Step 3: Implement** the CLI and the report; tune only the rules from Tasks 4–5 until the test passes without special-casing addresses.
- [ ] **Step 4: Run** — PASS. Record the report size; if it is over ~200 entries, tighten rules (Global Constraints) and rerun.

### Task 9: Apply, verify, document

**Files:**
- Modify: `game/pacing_fixes.inc`, `project/hle_map.txt`, `project/uc.cfg` (by the tool, `--apply`)
- Modify: `docs/testing/timing-handoff.md`, `docs/testing/timing-audit.md` (summary section)

- [ ] **Step 1:** `python -I -m tools.step_audit --prove --apply` — expected: every written record has `pass`. The report lists the rest with reasons.
- [ ] **Step 2:** `.\build.bat --no-install` — all tests pass. `ctest --test-dir build -R test_pacing_fixes` — PASS for the real table.
- [ ] **Step 3: Performance.** Stage the previous dist exe as `dist/uc_before.exe`, then run `cd scripts; py -3.13 -m uctest bench ../dist/uc_before.exe ../dist/uc.exe --demo fight2`. Expected: CI includes 0 or better. `demo check fight2` must still pass. If the demo is stale (combat check, memory `demo-staleness-pacing-change`), say so and skip.
- [ ] **Step 4:** At most one live A/B (`build/episode_probe.py 0 30|480 -2 30`) only if a record touches enemy movement or AI. Compare with `monkey_ab`-style metrics.
- [ ] **Step 5:** Manual review of report-only entries, highest confidence first. Confirmed bugs without a template get hand-written hooks in `game/game_pacing.cpp` with tests, as before.
- [ ] **Step 6:** Update the docs with counts (found / covered / auto-fixed / report) and the commands. Checkpoint; ask the owner about commit and installation.
