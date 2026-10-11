# S4 — Compile time of translated-code-sized output

Date: 2026-10-12. THROWAWAY spike (`tools/spikes/s4_gen.py`, `tools/spikes/s4/CMakeLists.txt`).
Machine: Ryzen 9 7950X (16 cores / 32 threads), 64 GB RAM. clang-cl 23.1.3, `/O2 /clang:-march=x86-64-v3`, Ninja, Release.
Toolchain ids used (winget): `Microsoft.VisualStudio.2022.BuildTools` (already present), `LLVM.LLVM` (already present), `KhronosGroup.VulkanSDK` 1.4.363.0 (installed in this run).

## Generated input

Each function: `REPS` blocks of byteswapped load/modify/store plus an FP multiply-add chain with a data-dependent branch, and a call to the next function. 200 functions per `.cpp`.

## Measurements (clean builds)

| Run | Functions | Blocks per function | Source | Output | Time |
|---|---|---|---|---|---|
| A | 9029 | 18 | 21 MB | static lib | **0.12 min (≈7 s)** |
| B | 9486 | 90 (5x heavier) | 108 MB | DLL | **0.42 min (≈25 s)** |

N = 9029 is the unique `bl` target count from S1; run B uses 9486 (the `stwu` prologue count, a better upper bound because
functions reached only through pointers are not `bl` targets).

## Verdict: PASS (threshold 15 min)

- Measured: 25 s for 108 MB of source, about 5x the instruction volume of `main.dol` text (≈968k PPC instructions).
- Not measured, treat as open:
  - The generated functions are uniform straight-line code with one branch each. Real translator output is branchier, has very uneven
    function sizes (a few huge ones), and the optimizer is superlinear in function size. A few huge functions can dominate wall time.
  - The DLL link in run B is not representative: the DLL came out at 0.33 MB because the linker dropped unreferenced code.
    A real link carries ~4 MB of code; expect seconds, but this was not measured.
  - Slower CPUs and LTO were not tried.
- Plan 2 re-measures with real translator output and keeps the hash-based cache plus per-file `/O1` for cold code as options.
- Spike CMake avoids `CONFIGURE_DEPENDS` on the glob (re-running `VerifyGlobs` failed in this setup). The main project does not glob.
