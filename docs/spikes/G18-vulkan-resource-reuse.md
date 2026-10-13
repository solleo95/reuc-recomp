# Native Vulkan descriptor and EFB image reuse

Controlled test windows now play audible game audio at owner's explicit request. Internal input/isolated saves remain unchanged.

## Descriptor pools

Each retired slot previously destroyed and created its descriptor pool per pass. New GPU regression fails after16same-capacity submissions. Pool remains slot-owned, resets only after fence completion, grows on capacity miss and retires on resize/shutdown. GPU grow/shrink/empty/two-slot/resize regression and Vulkan validation pass. Scoped review: no issues.

Visible PID35472 log `build/autotest/1791907863472720300/run.log`: pool creations settle at16, active28–30FPS, CPU render preparation remains roughly280–330ms/sec. Avoided churn confirmed; large FPS gain not established for this item.

## EFB copy allocations

Actual scene creates500–600GPUcopy images/sec while CPU texture decoding is0. Immutable versions retain correct source ownership, but expired versions were destroyed/reallocated even with the same extent/mips. Native free pool reuses only expired source versions with map-only ownership after slot fence references retire. New versions retain distinct ownership; reused images discard old layout/content and reset initialized=false. Shared chain validation runs on cold/reuse paths. Free pool bounded32images/64MiBactual Vulkan allocation bytes; shutdown frees all cached images.

GPU RED allocation-growth test, GREEN warmed12copies, retained old red version after new alternating red/green copies, malformed chain, resize and completion/clean-shutdown tests pass with validation0. Scoped review caught low memory-allocation error cleanup; expired map entry now erases before optional recycle insertion, so exception leaves no empty map entry. Recheck: no issues.

Visible audible PID6560, log `build/autotest/1791908313397670300/run.log`, shows active image allocations0–4/sec after warm-up and native render preparation approximately105–115ms/sec instead of300ms/sec. Active game typically28–30FPS; transition36anddeath-screen60excluded. `frame-1791908372543628-6560-27828.png` confirms scene/HUD detail retained. No FPS-unlock claim; simulation still uses original cadence. Audio overrun count10stabilizes through sampled battle; loading bursts/drift remain TD-042.

Full63CPUtests/build/dist/install and targetedGPUvalidation pass after the final exception-order refinement. Latest native build installed successfully.

## Build reliability

test_savefs PID-only temporary paths collide when Windows reuses PIDs and intentionally retained test folders remain. Reproduced failure onrepeat10atcreate_dir("/title"). PID+monotonic suffix fixes isolation;40repeat runs pass. No deletion of tests, plans or owner saves.
