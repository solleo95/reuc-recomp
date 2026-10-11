# S5 — SDK function identification

Date: 2026-10-12. Tool: `tools/sigmatch` (`--min-words 3`). Database: Dolphin `Data/Sys/totaldb.dsy` (10,549 entries), downloaded with the user's permission into `third_party/signatures/` (git-ignored, not redistributed; only the data file is used, no Dolphin code).

## Result

| Item | Value |
|---|---|
| Functions analyzed | 14,620 |
| Functions named by signature | **2,771** (0 ambiguous) |
| String anchors (functions that reference SDK-looking strings) | 14 |
| `project/hle_required.txt` — `core` names identified | **30 / 41 (73%)** |
| `project/hle_required.txt` — `optional` names identified | 4 / 7 |
| Output for the next plans | `project/identified_sdk.txt` (`<addr> <name> <lib> <object>`) |

Per library (named functions): `bte.a` 669 (Bluetooth stack), `homebuttonLib.a` 306, `gx.a` **145**, `os.a` **119**, `dvd.a` 78, `nand.a` 36, `mtx.a` 35, `sc.a` 31, `vi.a` 30, `ipc.a` 30, `wpad.a` 28, `wud.a` 38, `exi.a` 22, `fs.a` 22, `si.a` 20, `mem.a` 20, `axfx.a` 18.

## What the data says

- The checksum masks taken from the Dolphin source summary work: 2,771 functions matched with equal size, and one independent string anchor agrees (the function referencing "DVDReadAsync(): specified area is out of the file" is named `DVDReadAsyncPrio` by signature). That is evidence for the masks, not a proof for every name, and 240 `J3DGraphLoader.a` hits below are certainly false.
- The "0 ambiguous" figure of the tool is not meaningful: `sigmatch` lets the last matching prefix of a function win and does not accumulate candidates across prefixes. Treat the named count as an upper bound.
- **GX is not anonymous after all.** 145 `gx.a` functions are named, including `GXInit`, `GXBegin`, `GXSetTevOp` and `GXCopyDisp` (all four optional GX names were found). Call-level HLE of GX is therefore possible for most of the API, which weakens the argument in the plan for emulating the hardware interface (FIFO) instead. The runtime/renderer plan should decide with this table in hand.
- Some public names appear under their internal symbol (`__VIInit`, `__OSInitAlarm`); `hle_required.txt` accepts `A|B` alternatives.
- **The database also produces wrong names**: 240 hits in `J3DGraphLoader.a` and a `LexicalCast<…>` symbol next to the `%s:%d Panic:` anchor come from generic code that shares an opcode shape with other games' functions. A name is trustworthy only for the SDK libraries above and should be checked against an anchor or a neighbour before it is used for an HLE replacement.

## Verdict: PARTIAL (30/41 ≥ 70%)

Still missing (core): `OSInit`, `OSJoinThread`, `OSGetTick`, `OSReport`, `OSPanic`, `DVDClose`, `WPADInit`, `WPADRead`, `KPADInit`, `KPADRead`, `KPADSetPosParam`. Optional: `AXInit`, `AXAcquireVoice`, `NANDInit`.

Plan for each group (all are cheap, none needs Ghidra yet):
1. `OSPanic` and `OSReport`: the function at `803443B8` references `%s:%d Panic:` and is the `OSPanic` candidate; `OSReport` is the function it calls with that format.
2. `OSInit`: it is the only function that references the OS banner `<< RVL_SDK - OS …` through the library version table (`__OSVersion`) and calls `__OSInitAlarm`/`__OSInterruptInit` (both named); find it by call-graph.
3. `WPADInit`, `WPADRead`: 28 `wpad.a` functions are named around them (`WPADiHIDParser`, `WPADProbe`, `WPADSetDataFormat` …); the `WPADInit()` string is the anchor. Callers of `WPADiHIDParser` and the sampling callback give `WPADRead`.
4. `KPADInit`, `KPADRead`, `KPADSetPosParam`: the Dolphin database has no `kpad.a`. Find them as the callers of the named `WPAD*` functions (`KPAD` is a thin layer over `WPADRead`), then confirm by the KPAD banner (`0x80432BD0`). This is the function pair the mouse-aim work in the input plan depends on, so it gets verified by reading the code.
5. `OSGetTick`, `OSJoinThread`, `DVDClose`: tiny; match by shape (mftb / `OSJoinThread` is next to `OSCreateThread` in `os.a`; `DVDClose` calls `DVDCancel`).

Ghidra stays postponed (`docs/decisions.md`). Revisit it if steps 3–4 do not produce `WPADRead` and `KPADRead` with confidence.

## Reproduce

```
build\tools\sigmatch\sigmatch.exe --dol "S:\Games\Umbrella Chronicles\game\DATA\sys\main.dol" ^
  --dsy third_party\signatures\totaldb.dsy --cfg project\uc.cfg --required project\hle_required.txt ^
  --min-words 3 --out build\sig_3
```
