# GX ABI audit — RBUE08

Current continuation: native typed resource playback reaches730game-produced draws/2920vertices in12seconds; details and limits in G4-native-resources.md. Historical zero-draw measurements below describe earlier builds. GXDrawDone now uses real GPU completion; its old Nop note is historical. Audit gate remains PARTIAL for material/state/copy and prebuilt/inlined paths.

2026-10-12. Source: user's `DATA/sys/main.dol`. Local `xref --disasm`, `--callers`, generated static C++ cross references. SDK names from signature databases remain candidates until checked here. No external implementation copied.

## Confirmed boundaries

| Entry | Address | Input / result | Evidence and side effects |
|---|---|---|---|
| GXBegin | 80284FD0 | r3 primitive, r4 VAT, r5 u16 vertex count; void | 802850F0 ORs primitive/VAT; 802850F4 writes byte, 802850F8 writes count. Dirty flush first; SDK dummy flush strip can precede actual draw. |
| GXSetVtxDesc | 80283564 | r3 attribute, r4 descriptor; void | switch table 80427430; SDK VCD +0x14/+0x18, normal mode +0x520, normal/NBT flags +0x524/+0x525. Sets dirty state. |
| GXSetVtxAttrFmt | 80283B8C | r3 VAT, r4 attribute, r5 component count enum, r6 type, r7 fractional bits; void | VAT A/B/C at +0x1C/+0x3C/+0x5C, stride 4 per VAT. Dirty mask +0x5FC, VAT mask +0x5FB. |
| GXSetArray | 802841DC | r3 attribute, r4 guest base, r5 stride; void | NBT attribute 25 maps to normal 10. Emits CP array base masked to 30 bits and stride. Does not retain the array in SDK shadow state. |
| GXSetDrawDone | 802848AC | no arguments; void | Emits finish 0x45000002, flushes pipe, sets byte [r13-25328]=0 under disabled interrupts. Does not wait. |
| GXWaitDrawDone | 80284944 | no arguments; void | Sleeps on queue [r13-25336] until finish flag is nonzero. |
| GXDrawDone | 80284990 | no arguments; void | Same submit sequence plus wait. Currently HLE Nop; real renderer must replace it. |
| GXSetDrawDoneCallback | 80284C30 | r3 callback address; r3 previous callback | Loads/stores [r13-25324] under OSDisable/RestoreInterrupts. Previously unnamed signature gap resolved without Ghidra. |
| GXFinishInterruptHandler | 80284C74 | r4 interrupted OSContext; void | Marks finish flag=1, calls callback with no explicit arguments, restores OS context, wakes finish queue. No completion may run merely because VI tick occurred. |
| GXCopyDisp | 80285838 | r3 XFB guest address, r4 clear bool; void | Copy shadow +0x230..0x23C; address encoded as /32. Restores depth/color state after copy/clear. |
| GXCopyTex | 80285974 | r3 texture guest address, r4 clear bool; void | Texture-copy shadow +0x240..0x24C; copy-format flag +0x250. Copy destination and subsequent clear must remain ordered. |
| GXLoadTexObjPreLoaded | 802869A0 | r3 texture object (32 bytes), r4 cache-region object, r5 map id; void | Object words +0/+4/+8/+0xC rewritten with map register tags; optional TLUT callback. Shadow retains only texture size/mode words +0x564/+0x584, not all texture identity. |
| GXSetTevColor | 80287908 | r3 register id, r4 pointer to RGBA8 (4 bytes); void | Two BP register values emitted, SDK shadow does not retain color. Native boundary must retain these values. |
| GXSetTevColorS10 | 80287968 | r3 register id, r4 pointer to four signed 16-bit channels; void | Signed 11-bit register fields; same lack of retained shadow. |
| GXCallDisplayList | 80288684 | r3 guest pointer, r4 byte length; void | Dirty flush, optional dummy primitive, emits 0x40 + pointer + length. List contents never executed by translated CPU. |
| GXBeginDisplayList | 80288528 | r3 aligned guest destination, r4 capacity; void | Configures temporary CPU FIFO at 8061C2A8; save-context option at SDK +0x5F9, active flag +0x5F8. Dirty-state flush precedes recording. |
| GXEndDisplayList | 802885D8 | no arguments; r3 recorded length, zero on overflow | GXGetCPUFifo reads hardware-backed cursor through 80283158; 802832F4 returns temporary FIFO +0x1C count, 802832FC returns +0x20 overflow flag. Restores prior FIFO and optional SDK state except +8. |
| GXSetProjection | 8028873C | r3 float matrix pointer, r4 projection type (1 orthographic); void | Shadow type +0x528, six floats +0x52C..0x540; ortho uses matrix +0xC/+0x1C, perspective +8/+0x18. No generic depth conversion assumed. |
| GXSetProjectionv | 802887A0 | r3 pointer to seven float values; void | First float selects type; remaining six copied to shadow. |
| GXLoadPosMtxImm | 8028882C | r3 3×4 float matrix pointer, r4 matrix index; void | XF address=index×4, twelve floats emitted as six paired-single stores. No retained matrix shadow. |
| GXLoadPosMtxIndx | 8028887C | r3 array index, r4 matrix index; void | Emits indexed XF opcode 0x20; payload stores array index, count 12 and destination index×4. |

SDK state pointer is **[r2-8968]**, evidenced by all audited setters. A native replacement must preserve relevant shadow state or replace all readers consistently. Array bases, matrix contents, TEV colors and loaded texture objects cannot be reconstructed from shadow alone.

## Actual callers and inline paths

- 70+ static GXBegin calls, including UI libraries and game routines. At 80029D84: primitive=0x98, VAT=0, count=4; the following position/color/UV bytes are inlined stfs/stw to 0xCC008000. Vertex count is the boundary; no GXEnd call.
- GXCopyDisp is called from startup/framebuffer paths 80039864, 80039C50, 8003A6AC and 8003A77C. GXSetDrawDone is called at 8003A674. Runtime reachability remains to be measured.
- GXCallDisplayList callers: 8003CDE0 and wrappers 80366D3C, 80366E14, 80366E78, 80366FDC, 80367AC4, 80367FDC.
- **Bypass found:** 80366DA4 and 80366E04 (wrapper 80366D3C), plus 80366E68 (80366E14) directly emit list opcode/pointer/length without GXCallDisplayList. Replacing GXCallDisplayList alone cannot cover geometry. Candidate higher boundary is the resource/draw wrapper; its data layout and reachability need further audit.
- **Indexed XF path confirmed:** 8028887C. This is matrix data ingestion at a verified function boundary, not permission for a general FIFO interpreter.

## Native implementation ruling

Retain translated SDK setters that only maintain shadow; read verified VCD/VAT at GXBegin. Capture non-shadow state at verified call boundaries (arrays, matrices, textures, colors). Consume only descriptor-sized vertex data while a native primitive is active. Explicitly account for writes outside the primitive; no silent discarded command accepted as a correct frame.

Display-list resources need a native mesh/material import or a verified higher-level replacement. Generic runtime FIFO interpretation requires a separate design decision. This audit has **not** established full material/texture/TEV ABI coverage, runtime draw reachability or first-image acceptance.

## Task 2 result

`test_gx_io`: u8/u16/u32 truncation, f32 bits, f64 high/low order, paired-single float/quantized pairs, write widths, addresses, RAM separation and FIFO aliases verified against literal expected values. Existing helpers needed no rewrite.

New MMIO boundary regression failed on a 2-byte write starting at the final byte of a range. Root cause: bus validated only starting address. Shared lookup now validates the complete 1/2/4-byte access; out-of-range/invalid widths follow existing unmapped diagnostics. `scripts/dev.bat`: 40/40 pass after fix.

## Still to audit

## Runtime evidence (12 seconds, trace build)

`scripts/bringup.bat --seconds 12 --gx-audit --log-dvd`: exit 0, 719 timer retraces, 13 opens, 15 reads, 3079 FIFO bytes, zero unmapped accesses. GXSetDrawDone=1, GXWaitDrawDone=1, GXCopyDisp=3, GXBegin=0, GXCallDisplayList=0. The default thread's trace reaches GXWaitDrawDone and waits on the finish queue. Native GPU completion is required before measuring the first draw. No inlined display-list wrapper was reached in this run; static bypass evidence still matters later.

The audit logger records translated function entries and first GPR/FPR values in synchronized snapshots. Shared HLE replacements do not enter translated function bodies, so their calls are absent from these counters. Signature display names in logs may be wrong; `gx_symbols.txt` overrides the confirmed subset. GXSetDrawDoneCallback (80284C30) also matched an unrelated `AIRegisterDMACallback` signature; the finish handler's use of the same [r13-25324] slot confirms the GX identity.

This refines prior M1: runtime/timer/file-service bring-up passes, game render-loop progress remains unproven until GPU finish drives it.

### Remaining ABI work

### Native GPU completion integration

Native GXBegin/SetArray and DrawDone/SetDrawDone/WaitDrawDone thunks are connected. A CPU ABI fixture captures a literal three-vertex draw; a consumer completion posts onto the scheduler, sets the SDK flag, invokes the current callback once and wakes the SDK queue. Duplicate completion is ignored.

First real integration run reached an indirect target 8039E920 that was not registered as an entry. DOL evidence: previous instruction 8039E91C is blr; 8039E920..8039E94C is a complete leaf initializer. `entry 8039E920` adds the callable boundary without changing translated semantics. Ghidra unnecessary for this identified gap.

Six-second window run after that correction: exit 0, 360 retraces, 349 submissions and 349 actual GPU completions, zero validation errors, zero unmapped MMIO. Native GXBegin draws remain zero; 174867 bytes occur outside a captured primitive. This proves the former GPU-finish wait is resolved; the graphics state/list/input path still needs audit. No first game-image claim.

Five-second follow-up: 297 native completions, 588 GXCallDisplayList calls, **zero nonempty lists**. GXBeginDisplayList and GXEndDisplayList each called 592 times. DOL confirms that translated GXEndDisplayList derives size from the hardware-backed FIFO cursor, which the plain register mapping never advances. This identifies a concrete missing SDK replacement; absence of geometry must not be explained solely as an input gate. Native recording/resource import is required at verified SDK or higher game boundaries; no device cursor model or general FIFO interpreter added.

The bounded audit prefix records 75199 bytes during those recording sessions. First caller 8003F73C; first bytes `61 30 00 00 03 61 31 00 00 03 ...` contain state data. This proves nonempty CPU-generated content despite the zero hardware-derived length.

Native Begin/End resource writer replaces that cursor dependency. It records bytes into a checked guest buffer, uses translated verified dirty flush/GXFlush routines for SDK data generation, restores the optional SDK shadow while preserving +8, clears active flag +0x5F8 and returns complete 32-byte blocks. GXBegin/SetArray generate resource headers during recording; this never sends a live draw. No command execution or FIFO interpreter involved. Overflow returns zero length, restores context and retains the previously committed destination; RED/GREEN recovery fixture verifies that a subsequent live draw still works.

Six-second real trace with this replacement: exit0, 708 nonempty list calls / 79296 bytes and 357 GPU completions. Geometry remains zero: recorded resources still need native state/mesh/material import. Audit gate remains **PARTIAL**, no first-picture acceptance.

The transfer-only swapchain triggered color-attachment VUIDs in legacy rendering calls outside this backend, even with loader implicit layers disabled. Requesting supported color-attachment usage as well as transfer-destination usage removes those errors; backend still renders to its own attachment. Exact external caller not identified, so do not attribute it to a specific overlay.

Texture/TLUT full layouts, normal/texture matrix loads, channels/texgen/TEV/fog/indirect states, render-copy encodings, runtime display lists and draw completion/copy sequence. Task 1 remains in progress until these boundaries are proven.
