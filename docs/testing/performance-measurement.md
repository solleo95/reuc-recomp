# Performance measurement protocol

**This is the only way to judge whether a change makes the game faster or slower.** A single A/B of two builds (a wall-clock bench, a live FPS run, `deck_bench.py`
with two labels, the share of a function in a profile) is not evidence: the speed of a build depends on where the linker happened to put its code, and that alone
moves the result by about +-1% (measured below: one source, the same work, six code layouts: -1.1% .. +2.0%). The changes still worth finding are 1-3%, so
they drown in it. Mistakes this rule prevents: a "-1.8%" that was a lucky layout, a "+1.9% slower" for a change that in fact removes 2.6% of the work and
2.5% of the cycles, optimisations kept or rejected on noise.

## What is measured

The deterministic demo (`uctest demo`, `build/demos/*.demo`; `--deterministic --demo-play`) replays the same game seconds with identical guest work. During the
measured window the game reads the **hardware counters of its guest threads** (`runtime/perf_counters.cpp`, `UC_PERF_COUNTERS=1`, set by `deck_bench.py`; they
appear in the `[detbench]` lines):

| metric | what it is | depends on | resolution (Deck, fight4 10 s, measured) |
|---|---|---|---|
| **instructions** (user space, guest threads) | the work | the code only: not on layout, clock, load or other threads | 0.0013% between runs of one binary, the same in every layout (+-0.002%) |
| **cycles** (user space, guest threads) | the time the guest threads ran, independent of the clock speed | work, code layout, caches | 0.3-0.5% between runs; one layout against another -1.1..+2.0% |
| wall time of the window | time on the clock | everything, including waits for the renderer in the deterministic mode | does not follow the cycles (see below) |

**FPS follows the cycles**, not the wall time of the deterministic window: in the live game the Deck's guest thread is busy ~98% of the time
(docs/spikes/steamdeck-windows-validation-backlog.md, section 6), so with the same work per frame fewer cycles means a proportionally higher unlimited FPS. In the
deterministic mode the window also waits for the renderer and the virtual clock, so its wall time can stay put while the guest cycles fall (the destructor case
below: cycles -2.45%, wall +0.18%). The protocol does not see scenes bound by the GPU, the driver or a frame limiter, single slow frames (`UC_FRAME_LOG`, p99 of
`interval_ms`, belongs next to it when the change is about hitches), or scenes the demo does not contain.

## The quick check (the default): one run before, one run after

`python scripts/linux/measure.py BEFORE AFTER` — BEFORE and AFTER are `uc` binaries (or labels of `build/layouts`). The run of a binary is remembered by the hash
of the file (`build/measure/cache`), so the run before is made once per binary and the next change is compared with it without repeating it. It judges the **work**:

| verdict | meaning | what to do |
|---|---|---|
| LESS-WORK | instructions fell by more than 0.02% | the change removes work for sure: keep it. The size is exact (to 0.002%) |
| MORE-WORK | instructions rose by more than 0.02% | the change adds work: reject it, or find out why |
| SAME-WORK | within 0.02% | nothing is known about speed from this check (a change that does not remove instructions must be judged by the full protocol) |
| CHECK-TIME | the pair is slower in cycles by more than 2.5% (a layout alone does not do that) | run the full protocol before keeping |
| INVALID | the guest work of the two runs differs | not the same demo window |

The cycles and wall time of the pair are printed as information: one pair in one layout moves by +-1% on layout alone, so they are **never** a reason to reject
and never a reason to claim a gain. Instructions are not time (a removed instruction can be a cheap one, an added one a slow one); that is why the quick check
accepts only a change that is meant to remove work and why a large time worsening escalates.

## The full protocol: only when a time claim is needed

For a change that is not meant to remove instructions (a cache, a different data layout, fewer stalls), or when the size of a gain in time must be stated:

1. Build the state (`scripts/linux/build.ps1 -Target uc`), then `python scripts/linux/relink_layouts.py LABEL 1 2 3 4 5 6`: the same objects relinked six times with lld
   `--shuffle-sections` into `build/layouts/LABEL_s1..s6` (a second each). Do it for the baseline and for the candidate **with the same seeds**.
2. `python scripts/linux/measure.py BASE CAND --full` runs both builds in every layout (12 runs, the order alternates) and compares **seed by seed**: the ratio of
   one layout carries no layout effect because both sides have the same layout order. It reports the change in instructions, cycles and wall time with the 95%
   interval over the layouts. At least 4 layouts are required (fewer: INVALID).
3. Verdict (`scripts/linux/perf_stats.py`, `verdict()`): FASTER (the cycles interval is entirely below zero), WORSE (entirely above zero, or the work rose),
   WORK-ONLY (less work for sure, time not resolved: keep, state the instruction change), INCONCLUSIVE (nothing is known: do not reject, do not claim; add layouts),
   INVALID.

The time resolution is about +-0.5..0.8% for 6 layouts and narrows with the square root of the number of layouts.

**Never use** for a decision: a single A/B of two builds, one layout's cycles or wall time, the live `fight`/`perf` FPS (7-13% between runs), the share of a
function in the sampler profile across two builds (it moves with layout and profile; use the sampler to *find* what to optimise), PGO-trained or untrained builds
compared with each other (the profile is part of the source state: train once, then compare), the old "Mcycles" CPU-time column.

## Validation (2026-11-03, Steam Deck, fight4 10 s, state "X" = M15 + destructor HLE)

The statistics have unit tests (`tests/test_perf_stats.py`: pairing removes a +-5% layout offset, a synthetic gain is FASTER, a loss WORSE, an unchanged build is
called a difference in fewer than 8% of 400 simulated experiments). The real rig was checked with controls whose answer is known; each ran the full protocol with 6 layouts:

| control | what differs | instructions | cycles (95%) | verdict |
|---|---|---|---|---|
| determinism | the same binary, 3 runs | 107 528.7 / 107 527.8 / 107 529.2 M (spread 0.0013%) | 55 902 / 55 759 / 55 636 M (0.48%) | the limit 0.02% for instruction counts holds with a 15x margin |
| no-op, small | a never-called 400-byte function added in `sdk_math.cpp` | -0.003% +-0.006% | -0.07% +-0.77% (layouts -1.11..+0.94) | INCONCLUSIVE: no false claim |
| no-op, large | a never-called 6 KB function added in `gx.cpp` | +0.001% +-0.006% | +0.35% +-0.81% (layouts -0.80..+1.07) | INCONCLUSIVE: no false claim |
| positive: added work | 150 volatile loop iterations in `hle_ResMeshDraw` | +0.697% +-0.010% | +1.16% +-0.70% | WORSE, found and measured exactly |
| real case: the destructor HLE | `hit_buffer_destruct` against the translated destructor (profile estimate: 1.5% of the thread) | **-2.639% +-0.002%** | **-2.45% +-0.50%** (layouts -1.62..-2.97) | FASTER; wall of the window +0.18% +-0.49% (it does not follow) |

The real case is the one the old method got wrong: a wall-clock A/B of the same two states, with a retrained PGO profile on each, said +1.9% *slower* (ABBA,
sd 0.3%), because a function that was not touched moved by +1.4% with the code layout. Both facts are real; only one of them is the change.

## Limits (what "reliable" means here)

No measurement is 100% certain; this one is controlled: the work metric is exact (0.001%), the time metric has a stated resolution, the verdict says "inconclusive" or
"time not resolved" instead of guessing, and a loss is declared only when the work grew or the cycles interval over six layouts is entirely above zero. A time
effect below the resolution is reported as such, never as a loss. The result holds for the demo that was used and for guest-bound play; a change that matters only
in other scenes needs a demo of them, a change about frame pacing needs the frame log.

## Windows

The same protocol with these differences (**not yet validated on Windows: it needs a Windows build, which waits for the owner's go-ahead**):
- cycles: `QueryThreadCycleTime` of the guest threads (`runtime/perf_counters.cpp`, the same `UC_PERF_COUNTERS=1` and `[detbench]` fields; instructions are 0 there);
- instructions: this CPU supports `InstructionRetired` in ETW (`wpr -pmcsources` lists it); an elevated session counts them for the game process during the
  window. Without that, a Windows verdict rests on the cycles of the full protocol and its resolution;
- layouts: `lld-link` has no section shuffle; a symbol order file (`/order:@file`) with a seeded permutation of the function names from the linker map gives the
  same effect (needs function sections, `/Gy`, on in Release);
- the validation table above has to be repeated there (determinism, two no-op controls, the added-work control) before a Windows verdict is trusted.

## PGO (the optimizer profile)

Changes are developed and compared on builds **without** the profile (`scripts/linux/build.ps1 -NoPgo`, folder `build-linux-nopgo`): the instruction count of a work-removing
change does not depend on it, and retraining after every change would cost about an hour each. The profile is recorded **once, on the final tree, before the release
build**: `pgo.bat` (both), `pgo.bat linux` (`scripts/linux/pgo_train.py`: the Deck, three deterministic demos, a clean profile) or `pgo.bat windows` (`scripts/pgo.py`).
A profile belongs to the source it was recorded from: after a change to code that the profile covers, the changed functions are compiled without it (measured: the same change
with a stale profile was +3.8% in cycles). Then check the release candidate with the quick check against the same tree without the profile (the profile must not add work or cycles).
ThinLTO needs no training; it is a link-time optimisation and works on top of the profile.

## Work moved to other threads

The instruction and cycle counters see only the guest threads, so a change that moves work to helper threads would look like a gain in instructions without being one. Judge it by the
**guest serial time**, `guest_serial_ms` in the `[detbench]` lines: the wall time the baton was held, which includes everything the guest thread waits for inside such a call. With one
binary and a setting that turns the helpers off (an environment variable), `python scripts/linux/measure.py BIN BIN --toggle --env-before NAME=0` runs the two arms in turn (no code
layout differs, the noise is the run-to-run 0.3-0.7%; `--repeat 4` and more gives a verdict with an interval). Tried and rejected this way: the AX voices mixed on three helper threads
(exact, 82% of the frames, but the mixing is about 150 microseconds and waking sleeping threads costs about 50: no gain).

## What the instruction count does not weigh

An instruction counts as one whatever it costs. Pairs that the processor fuses (a compare of memory with zero and its branch) and perfectly predicted branches are nearly free in time, and
cache misses, dependent chains and divisions cost many instructions each. So: **a drop in instructions is real work removed, but a change made of cheap instructions may not show in time, and
a rise can be harmless or ruinous** (the unconditional rollback marks: +7.07% instructions, found at once). Use the instructions to accept or reject by sign and size, keep the cycles of each
pair in view (they are printed), and confirm the sum of a series with `--full` on the release candidate. To find where the time goes (not the instructions), profile `--event cycles:u --period 800000`.
