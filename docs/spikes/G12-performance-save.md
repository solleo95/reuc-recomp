# G12: dialog performance and save creation

2026-10-12. Current executable/profile: RBUE08, Russian asset translation.

## Performance evidence

Same20-second trace invocation: `scripts\bringup.bat --seconds 20 --validation --skip-intro --gx-audit --log-dvd`.

Before:7–8presentations/s at the save dialog,1372–1568texture creations and CPU decodes per second,665–720ms/s spent inside renderer preparation/submission. Logs retained in `build/texture-baseline.log`.

After:57–60presentations/s, zero steady-state CPU decodes; late dialog interval228–240texture creations/s and218–279ms/s renderer time. Logs retained in `build/texture-optimized.log`. This measures presentation throughput on the current machine with ordinary validation; gameplay/high-refresh performance is still open.

CPU cache compares physical address, extent/format/mips, exact encoded bytes and palette format/content. Returned pixels are immutable versions; queued packets retain old versions. LRU storage bounded by64MiB and4096entries. GPU slots reuse immutable versions, prune after real slot fence completion, upload only new images, mark initialized only after successful queue submission, and retain unsubmitted uploads across acquire retries. Descriptor pools remain per batch (TD-037).

RED: new CPU cache API failed to compile; content/alias/palette/eviction tests then passed. GPU fixture verifies unchanged-slot reuse alongside replacement texture, resize, invalid-resource recovery and alpha sampling. Eight GX CPU tests and ordinary-validation GPU fixture passed. Scoped Luna review reported no issues.

## Save creation diagnosis

Owner clarified the symptom: creation screen remains visible; screenshot4 shows the saving warning. Installed `saves/title/00010000/52425545/data/BIO_UC0.DAT` existed with zero bytes. Original installed files preserved.

An isolated native executable/save root reproduced creation through ordinary mouse/A input. Before fix, NANDSafeOpen called ISFS_GetAttr, then ISFS_CreateFile at802ABE20 failed with−101; caller's attribute argument was00FFFFF2. DOL802A7D4C..802A7D68 proves attribute and owner/group/other permissions use four32-bit output stores (owner32/group16 remain unchanged). Native boundary incorrectly wrote these four fields as bytes, leaving stale stack data for subsequent32-bit loads.

RED: output32-bit values prefilledDEADBEEF exposed stale bytes. GREEN: fixed32-bit output stores and all four bounds checks; invalid final output must leave earlier outputs unchanged. Async output fixture also uses the verified ABI.

Real isolated run after fix wrote21248bytes intoBIO_UC0.DAT, closed both temporary/source descriptors, renamed the temporary file successfully, removed its temporary directory, and wrote61600bytes of companion data. Saving proceeded into a new unknown-entry trap at8030810C. Previous instruction80308108 isblr;8030810C begins a buffer-processing leaf with an indirect switch at80308134. Added verified entry to static translation configuration. No save completion bypass, fake successful write, or runtime code execution added.

Trace-only native ISFS result/callback diagnostics and mouse press coordinates are enabled in UC_TRACE builds; release build omits them. Current automated helper is an ignored build diagnostic, restricted to its child PID/foreground window; unrelated windows are never clicked. Tests use separate save roots.

## Compatibility follow-up

Copied release executable also verified from `C:/Windows/Temp`:12seconds, exit0,720retraces,623presentations,19opens/63reads, all workers joined, zero unmapped MMIO or ordinary validation errors. Log: `build/installed-g12.log`.

Final verification: static translation has15927functions and zero unresolved switch branches. Full `build.bat` passed55/55CPU tests, staged `dist` and copied it to the requested installation without deleting files. Reloading the actual isolated21248-byte save succeeded:20-second ordinary-validation run exit0,1200retraces,25DVDopens/86reads, zero unmapped MMIO/traps/VUID. Native GPU readback shows the Russian New Game/load-slot menu; creation and subsequent reload confirmed, gameplay remains unchecked. The original installed zero-byte save was preserved and can still trigger the game's damaged-data dialog.

Owner requirement added to design specification: every region/revision/language and translated asset/font encoding. Current profile still binds RBUE08 DOL addresses, locale rules and NAND home; each new executable revision needs verified ahead-of-time translation/HLE mappings. Existing files and plans preserved.
