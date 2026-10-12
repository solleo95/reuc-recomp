# Native EFB and display-copy boundary

2026-10-12. User screenshot shows white/gray rapid flicker and approximately109 Vulkan presents per second. Earlier single early black readback did not describe sustained output. New audit captures both the first presented image after geometry and a later image after120 presents: first black, later white. Recognizable-game-image acceptance remains pending.

## Cause and native change

The renderer previously cleared and presented on every GXfinish. Six-second traces reach roughly700 finishes but350 GXCopyDisp calls. Finishes are GPU synchronization boundaries, so those extra clears/presents destroyed intermediate EFB contents.

Verified DOL boundaries: GXSetCopyClear802855C8 takes an RGBA pointer in r3 and24-bit depth in r4; GXCopyDisp80285838 takes the XFB address in r3 and clear flag in r4. Two new native HLE mappings bring the total to107.

All draw batches now target one persistent Vulkan EFB. GXfinish preserves it and returns actual GPU-fence completion to the SDK. GXCopyDisp snapshots it, then optionally clears for subsequent drawing, and presents the snapshot. Copy completion does not set the SDK finish flag or call its callback. Owned packets retain color/depth/XFB metadata. No PowerPC execution, FIFO/list opcode interpreter or device model added.

The scoped Luna review identified EFB loss during swapchain resize. A new GPU regression reproduced it; EFB ownership now survives swapchain recreation. Its initial internal extent remains fixed and native blits scale output to the resized swapchain. The GPU fixture checks retained triangle pixels after960x540 resize, two finishes yielding one present, copy-before-clear, subsequent clear pixels and once-only native completion.

## Verification and limits

Eight GX-targeted CPU tests pass. Real Vulkan fixture with ordinary Khronos validation passes. Six-second game trace before the resize follow-up:368draws/1472vertices,698finishes/697reported completions,354copies and354actual presents; exit0. First presented geometry readback serial34 is black; later readback is white, matching the user's reported blank output. These snapshots do not prove visual flicker is entirely gone.

Final build.bat passes51/51tests, stages dist and copies to S:/Games/Umbrella Chronicles. Installed release exe launched from C:/Windows/Temp for6seconds with ordinary validation: exit0,361retraces,19opens/63reads/13637056bytes,370draws/1480vertices,700finish submissions/699reported completions,355copies and355presents, zero unmappedMMIO. All guest/file workers join; the final outstanding fence is retired during renderer shutdown. No validation errors in this ordinary-validation run. GPU smoke separately covers the resize regression.

Synchronization validation found an acquire/layout dependency error; TRANSFER source stage fixes that hazard. Remaining PRESENT_AFTER_WRITE errors reference vkCmdEndRenderPass, which this renderer never calls. RTSSHooks64.dll was observed in the process even with implicit Vulkan layers disabled. Overlay ownership is suspected; isolation remains open as TD-035. No validation suppression or global overlay setting change.

TD-034 records copy-time presentation pending VI-selected XFB resources, crop/scale/filter/gamma, depth, YUV/readback and recorded-copy support. TD-014 retains texture/TEV/material gaps responsible for missing game-image parity. This completes the erroneous per-finish clear/present fix, not the full renderer plan.
