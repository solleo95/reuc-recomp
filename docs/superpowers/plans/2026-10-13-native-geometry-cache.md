# Native geometry cache

Goal: reduce repeated CPU vertex decoding in the train scene while preserving mutable assets and current pose/material state.

1. Add decoder regressions for cache reuse, encoded/array mutation, current viewport/cull/pose bindings, invalid arrays and bounded storage. Confirm RED before implementation.
2. Collect referenced indexed-array ranges only on cache misses. Cache complete owned mesh geometry with exact encoded bytes and source-range snapshots; validate every hit. Bound retained storage to 32 MiB and evict least-recently-used entries. No guest immutability assumption.
3. Parse mesh asset records once into typed native geometry and position/normal bindings. Resolve current indexed matrices, material and viewport at every call. Reset on runtime initialization; publish hit/miss counters atomically for profiling.
4. Verify CPU regressions, build/install, then visible skipped-intro train route using isolated saves and internal input. Measure active gameplay intervals, excluding tutorials/death screen. Review this item once and record evidence/debt.

Affected: renderer/gx_vertices.{h,cpp}, runtime/hle/gx.{h,cpp}, game profile output, decoder/mesh tests. No owner input, save mutation, device model or runtime command interpreter.

Implementation evidence: initial primitive-level cache regressed to 11–13 FPS through excessive misses/allocations; replaced before acceptance. Whole-mesh cache passes source/encoded mutation, current pose/viewport, source-range reset and failed-import tests. Full 61/61 tests and build/install pass. Visible PID13448 run (`build/autotest/1791903788189148000/run.log`) shows active battle intervals 28–30 presentations/sec, 230 initial mesh misses and continuously growing hits with no further misses. Scene confirmed in `frame-1791903855434563-13448-32480.png`. Final death-screen 60 FPS excluded. Scoped review: no issues. White highlights remain separate material-fidelity work; audible diagnostic output is muted.
