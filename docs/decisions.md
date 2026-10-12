# Decisions and fallbacks

Open items the project agreed to revisit. Newest first.

## GX at call level, not at the FIFO (decided 2026-10-12)

- The SDK's GX functions are identified (145 `gx.a` names) and `GXInit` already runs translated. The renderer plan replaces GX calls with a state tracker feeding Vulkan;
  the write-gather FIFO stays a discarded sink until then (TD-014). A FIFO-to-Vulkan parser is the fallback if call-level coverage proves incomplete.
- Ghidra: still not needed after bring-up M1. The six identifications the signatures missed (`OSInit`, `OSPanic`, `OSReport`, `KPADInit`, `KPADRead`, `WPADInit`) were found by call-graph work, and the retrace/scheduler/DSP functions needed later were found the same way.

## Ghidra (decided 2026-10-12: not installed yet)

- **Status:** not installed. The user chose to postpone it.
- **Return to it if:** SDK/function identification in the analyzer (plan `2026-10-12-analyzer-translator.md`, Task 8–9 gates) cannot name the
  GX, KPAD/WPAD, OS-threading, DVD or AX entry points, or a translated function behaves wrongly and the guest code needs a second
  independent reading.
- **What to do then:** ask the user, install Ghidra + JDK + a Gekko/Broadway loader plugin (verify current plugin names and DOL support first),
  cross-check function boundaries with decomp-toolkit (`dtk`), and store the results as data in `project/uc.yml`, not in the repository as a Ghidra project.
- **Status after the analyzer plan (S5):** signature matching named 30/41 required SDK functions (PARTIAL), so Ghidra is still not needed. Revisit if the WPAD/KPAD functions (`WPADRead`, `KPADRead`) cannot be found by call-graph work (see `docs/spikes/S5-identification.md`).
- **Not a build dependency.** `build.bat` must keep working without it.
