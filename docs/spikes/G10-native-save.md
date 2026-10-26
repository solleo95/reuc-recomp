# Native save-file service

2026-10-12. Replaces the failed startup filesystem gate with actual Windows files; preserves the game's missing-save/create-data flow.

## Boundary and evidence

Game save check80134D60 calls NANDGetHomeDir802ACAC4 at80134D80. The SDK requires its r13−25056 initialization word to be2. Previous0 returned−128 and led to the unknown-error dialog. Native NANDInit802AC778 creates/verifies the title home and writes the verified home/current-directory SDK buffers804291C0/8061F6C0. Repeated initialization preserves the current directory. Ordinary NAND wrappers remain translated.

ISFS entry points are verified by DOL disassembly, not signature names: CreateDir802A77C4 / CreateFile802A83BC have different operations despite incorrect automatic names; Read802A87E8 / Write802A88AC are also distinguished by their actual tail calls. GetFileStats802A873C publishes two BE32 values. GetAttrAsync802A7DA0 uses callbackr10 and userdata at originalSP+8. Async results are queued after actual I/O and delivered under the scheduler's interrupt context.

Modes and public call shapes cross-checked against [libogc isfs.h](https://github.com/devkitPro/libogc/blob/master/gc/ogc/isfs.h). ReadDir capacity is count×13 bytes; entries advance by actual name length plus NUL, not a fixed13-byte stride. Cross-check: [FS ReadDirectory implementation](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/IOS/FS/FileSystemProxy.cpp). This is ABI research only; no emulator implementation or device model is linked into the port. The scoped review's fixed-stride finding was rejected against that evidence; a two-name regression test protects packing and capacity truncation.

## Host storage and preservation

Root: `exe_dir()/saves`. The title's virtual home `/title/00010000/52425545/data` maps below that root. Native handles perform read/write/seek/stats/list/usage/rename. Relative/traversal paths, ADS, DOS device names, reparse points and hard links are rejected. Ancestor handles deny write/delete sharing until each path operation finishes, and stay alive for an open file; leaf handles use OPEN_REPARSE_POINT and handle-based validation. Backup copies read the validated file handle rather than reopening an unchecked path. Service archive directories are inaccessible through the SDK, case-insensitively.

Rename has a narrower namespace-write phase: the source handle is opened/validated while ordinary strict pins remain alive, replacement directory handles keep delete sharing denied before the strict pins close, and native NtSetInformationFile targets the pinned parent by handle with a relative leaf name. This allows kernel namespace writes without reopening the source path or a full destination path. A FILE_READ_ATTRIBUTES-only directory handle was insufficient to establish a write-sharing lock; GENERIC_READ plus a real conflicting-write regression now covers it. [Microsoft FILE_RENAME_INFORMATION](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_rename_information) documents relative RootDirectory handles and the sharing/access requirements. Replacement, one-character leaf names and empty-directory archiving have native filesystem fixtures.

Writable opening archives existing nonempty content under `.backups`; replacing a rename destination archives it first. Logical deletion moves into `.deleted`, retaining bytes. Empty-directory deletion is supported; recursive SDK deletion remains an explicit fidelity gap (TD-040). Native SDK asynchronous I/O currently runs under the guest baton with deferred real completion (TD-039); owner/permission metadata remains provisional (TD-040).

## Verification

RED→GREEN tests cover persistence after reopening, actual missing/existing errors, write permissions, bounded seek without position mutation on rejection, malformed guest buffers without filesystem writes, links/path escape rejection, parent-move refusal during an open file, backup/archive preservation, usage/listing, idempotent NANDInit, GetAttrAsync stack/outputs, packed two-name ReadDir and truncation, deferred read callback with real bytes and interrupts disabled.

Initial trace run:12seconds, ordinary Vulkan validation, `--skip-intro --gx-audit --log-dvd`, exit0;718retraces,19DVDopens/63reads,13658draws/54632vertices,124copies/presents, zero unmappedMMIO. GPU readback now says **Save game data not found. Create data?**. It establishes that the old failure gate is removed; it does not establish completed save creation or movie playback.

After correctness changes, the fixed120th-present capture repeatedly caught a near-black frame. Diagnostic-only late capture now also waits15seconds; a20-second run captures the real missing-save/create-data dialog at serial460, exits0 with1201retraces,19opens/63reads, zero unmappedMMIO. This distinguishes capture timing from the final dialog; it does not establish full material parity. For a late audit screenshot, use a run longer than15seconds.

Full build.bat after review corrections:54/54 tests pass, dist staged and copied to <install dir>. Installed release launched from C:/Windows/Temp with `--seconds 12 --validation --skip-intro`: exit0,725retraces,19opens/63reads/13637056bytes, zero unmappedMMIO, all guest/file workers joined. Saves resolve beside the exe, not under the foreign working directory. An earlier launch with trace-only `--gx-audit` correctly refused before startup; it was repeated with release-supported flags. Ordinary validation is used; unresolved synchronization-validation/overlay issue TD-035 remains open. Native input is the next prerequisite for answering the dialog; white placeholders/material gaps remain under TD-036.

Final installed release after strengthened sharing pins/handle-relative rename: same12-second foreign-CWD check exits0,692retraces,19opens/63reads, native home ready, zero unmappedMMIO/ordinary-validation errors, all workers joined. Full54/54 build and dist copy succeeded immediately beforehand. No complete game-save-format acceptance or final renderer review claimed.
