# I1 — native action boundaries, RBUE08

2026-10-13. Investigation in progress; candidate semantics remain gated before production replacement.

## Corrected effective addresses

`800684F4..8006850C`: lis8045 followed by signed addiA808 yields **8044A808**, player binding stride104. `800684DC..800684EC`: lis8044 + signed addiA818 yields **8043A818**, channel manager. Earlier plan notes incorrectly treated low halves as unsigned. Probe fixture reproduced the wrong-address failure, then passed after correction.

`8003FB98`: mask index0..19 at object+4+index*4, disabled when byte object+84+index has bit0. Initialized live masks observed in build/autotest/1791921418105633100/run.log:

```
0400 7F1F 2800 2800 4004 0008 2800 0400 2800 0010
2C00 5000 6C00 0008 0004 0001 0002 0010 1000 7F1F
```

Bindings4/5 selection paths verified at80068504/80068540; positive/negative index step, skip unavailable entries. Binding0 B0400 reaches weapon state machine8006C268; actual shooting still needs an unpaused live test.

## Validated readonly snapshot

`--input-probe` off by default. KPAD observer registered before guest execution, cleared after join; runs under scheduler baton. Mapped-span validation precedes all reads. Two parent pointers from r13-27464; runtime r13=806CA620. Live parents80E0C0E0 and80E4A900. Fields parent+630,+790,+794,+798; six words weapon(parent+48)+480..494. Raw field names retained until semantic verification.

At first train tutorial: player0 parent+794=1, player1=0, weapon0/currentword48C=15/state490=1. Disabled bindings4..7=1 during tutorial. B test while tutorial visible leaves15 unchanged: this correctly demonstrates unavailable action, not shooting acceptance. Screenshot frame-1791921874737407-10836-12858.png confirms the tutorial overlay. Do not report its60 presentations/sec as battle FPS.

Probe caps128 records of changes/button edges and keeps counters afterwards. Fixtures cover disabled mode with null memory, valid bindings/player reads, MMIO/end-of-MEM1/r13-underflow unavailable objects, cap and unchanged registers/memory. Input/KPAD observer tests cover invalid buffer preserving edges and no callback, valid callback preserving KPAD ABI.

## Reload candidate

`8006C268` weapon update: state0/1 motion recognition at8006C3E0/8006C4B4; getter80067CA4 actually returns parent+4F8+EC (hold-mode timer), **not health**. Positive timer check precedes8006D980. State3 actual shot8006D3C4 decrements48C; empty amount invokes8006D980 at8006C584. State2 polls animation80150AB8 then8006DABC returns to state1.

`8006D980` validates player index and magazine capacity801D431C, inventory801BD014; requests normal reload animation80150474 using descriptor duration38/58, event8006CA1C(id3), playerflag40000000, writes48C and state2. Verify through original empty-magazine reload first; native R must respect the same caller/context/state checks.

Live confirmation: build/autotest/1791922149911940300/run.log, original B presses after8 tutorial acknowledgements. Amount48C15→0→15, state490 transitions through firing3/4, Reload2 and Ready1; next shot15→14. Reload2 samples2253/2257; subsequent firing sample2270, ready sample2334. This verifies8006D980 as Reload through the original auto-reload path and normal animation wait800150AB8. Native R acceptance, full-magazine/finite-reserve behavior remain separate checks.

## Pending gates

Exact menu/loading/paused/QTE predicate; Grenade and Knife consumer ABI/direction90°; successful shot and reload animation/limits; grenade inventory/cooldown; QTE challenge preserved. No global motion override and no fake accelerometer input installed.

## Native action owner boundaries

Owner=parent+4F8;801AB350(owner,allow_input) invokes normal gesture updates, checks FCplayer, cooldowns F0(Grenade)/F4(Knife), EC hold-mode timer and F8 knife sequencing.801AB860(owner) reads inventory slot3 via801BD014, launches projectile type5, original event/animation, decrements inventory801BCF68, sets F0 from descriptor5+30. Native G/chord uses this entry after the original allow_input gate. Host mode disables only the old binding7 lookup at return address801AB590 so cleared combos cannot throw through raw A+B.

801ABA00(owner,channel) reads semantic processed vector from owner+4+channel*74+C/10; weapon descriptor6, effect name803E0348=`knife01`; original collision and animation retained.80394548 computes atan2 in256-angle units;806C7584=2π/256 and806C7588=π/2. Host vector(dx,dy) maps to processed(-dy,dx), so rightward(1,0) yieldsπ/2 (90°). No accelerometer fields required.

UI manager804B5CC8: enable+1D4A0,12 slots+1D4B0 (source801483D8/table803F9D98). Live title/save menus precede train HUD slots2/3. **Correction:** slot6 registration persists during gameplay and shares a no-op update target with slot3; its registration alone does not prove a tutorial/modal window. Native run1791925748100279700 exposed the erroneous Menu classification, fixture RED→GREEN after excluding draw-only6. Slot11state0 is continuously registered; positive state indicates its modal route. Actual tutorial visibility needs a separate predicate; original child allow_input gates remain authoritative for semantic calls. Game_input fixture uses object state+794 and active slots, not FPS/timers. Live native route must still validate context boundaries before acceptance.

## Diagnostic route blocker

1791922676798609500 run reached an uncovered GoalOverlay virtual entry80146A04 during live firing, before pause test. Original instructions80146A04 `addi r3,r3,-8`,80146A08 tailbranch80146960; preceding801469FC..80146A00 is a separate complete this-adjustor thunk. Added exact static entry to uc.cfg; no runtime fallback. Rebuild/replay required before accepting pause evidence.

Live native controls1791926726677724300: original weapon/owner pointers match snapshot (80E0C128/80E0C5D8). Fire amount15→12; R accepted and amount12→15/state2. S selection changes weapon0→2 and amount50. G accepted by actual inventory decrement; heldV produces6 accepted original Knife calls over1second. Next effect hits existing RGBA8-only EFB destination guard; exact format capture pending. Required tutorial pages must be acknowledged with Interact; native-context alone does not distinguish the tutorial's registration from visibility. Global game-mode getter801FC010 reads80596460+14C; mode3 gates original scene activity, but current tutorial remainsFFFFFFFF, so this gate alone is insufficient for tutorial visibility.

## Effect rendering and modal gate corrections

1791927270205426800: effect destination640x360, format27, caller800290D0. [libogc GX header](https://raw.githubusercontent.com/devkitPro/libogc/master/gc/ogc/gx.h) identifies GX_CTF_A8; original80285398 uses8x4tiles,32bytes/tile, copy control70. Native Vulkan-owned copy uses alpha replication through an image view; no CPU readback. GPU smoke checks alpha77 across RGBA and prevents recycling an alpha view as RGBA. Recorded-resource fixture checks captured A8 format and restored SDK/live destination.

1791928573871338000: subsequent native effect draws primitiveA8,2vertices/2indices, callsite8035BFC4. Existing decoder already expands GX_LINES/GX_LINESTRIP into pairs. [Vulkan LINE_LIST](https://docs.vulkan.org/refpages/latest/refpages/source/VkPrimitiveTopology.html) now uses a separate pipeline key; GPU fixture checks pixels, strip equivalence, malformed odd count and triangle pipeline reuse. Line width remains1physical pixel; GX width/texture-offset parity is unimplemented.

1791928776899701300: Reload/Grenade accepted and7 Knife calls; no previous effect crash. Capture frame-1791928833451112-16824-26906.png shows a damage tutorial, so29..30 presentation samples are not accepted battle FPS.

Original800674F0 calls800F3E28 before enabling player input. That gate blocks when scene+6BCC(float)>0, scene+6BD0(signed)>0, or either fade value>0.9998999834. Fade getter80041174 computes elapsed/duration, caps at1 and reverses on byte+C; observed constants read from original DOL. Native context now mirrors these validated read-only predicates, preserving Interact for modal acknowledgement and canceling gameplay requests. This is an additional aggregate suspension gate; exact tutorial/QTE panel identity remains unverified.

## Pause input and object

801FC4F4 constructs the pause owner at80596448+18=80596460 via801FB95C; its vtable80408F58 points to801FBAAC. The separate object at+178 uses a different vtable and is not this pause owner. At801FBD5C, state+148 must be1 and inhibit+158 zero; binding9 (live0010/Minus) triggers801FC03C, which performs original freeze/UI/audio work and writes state2. Native Esc now projects0010; old1000/Plus projection reached no pause consumer. Snapshot reads owner+148 and+14C separately; pause_state2 is modal regardless of pause substate. Correction: alternate binding9 in generic UI802355E0 does not establish resume; actual pause panel80231628 calls801D7D2C, whose Back lookup is index11. Paused and ordinary Back therefore retain1000. Probe fixture deliberately seeds the other object with99 to prevent wrong-owner regression. Live resume verification pending.

Game-boundary tests now cover cleared special bindings, logical Interact/Fire rebinding, one grenade for G+chord, unavailable one-shot disposal, suspension/QTE cancellation, and gesture priority over V in either event order. Reverse order initially let the held-V down event overwrite the mouse direction; selecting pulse gestures before physical holds fixed the regression.

Context release regression: projector previously interpreted canceled releases only through the new context, dropping Pause0010/Fire0400 when entering Menu. Fixture expecting410 failed; release projection now includes the original action across context cancellation and excludes any bit still held by another logical source. Original SDK receives actual up edges; no sensor/device state introduced. Live impact under verification.

## Native motion-QTE counter

Original80171E58 resets gesture struct+10(count) and stores mode+14.80171D38 increments+10 on qualifying movement and returns it. Four player callers are verified:800672C0/80067310(mode6301, threshold5) and800673A0/80067400(mode2, thresholdparent+79C), using parent+5F8 or+610. Each branch requires flags798&2. Original mode2 clears that window bit and advances threshold by3 after a step; original animation/timing/failure logic remains compiled.

Native replacement is scoped by exact object, player0 and return address; all other shared helper callers continue from80171D3C with the original statically lowered stwu. Real host motion reversals produce discrete qte_shake events; only the verified QTE window adds them to its semantic count. No IMU metadata or global motion override. Focus/config/context cancellation and movement outside the window discard pending steps. Fixtures confirm one gesture→one count, no-input/no-focus/outside-window→no progress, and no direct QTE success/state mutation. Live encounter verification pending.

Initial host gesture threshold:24raw units/leg,600units/sec minimum, reversal dot<−0.5,100ms leg window. SDL timestamps are preserved; controlled v2 events use monotonic publication time. Pure fixture with one consume/event versus batched consume produces equal step counts. DPI calibration and broader device/encounter acceptance remain open.

Actor/window regression fixtures now cover yielding original bodies and selection callers switching to a different player object: old wrappers leave the new player's requests queued. QTE token includes mode630 and target79C; changing either resets a half-gesture and pending steps. Old actor QTE objects return their existing count without sensor fallback. Final core build72/72CTest and NoActivate Vulkan smoke passed before config work.

## Tutorial object identified

Original HUD manager+94 child+22B8 =804B5CC8+234C calls8024FD6C. Resource string block803E51A0 names tutorial.csv, TRL.brlyt and TRL_open/loop/next.brlan. This links the actual tutorial subsystem without relying on signature names.8024F558 returns busy exactly when object+48 differs from00100000;8024F570 uses the same terminal-state check. Opening completion writes40000000; close completion writes00100000. Asset-ready byte+5 gates original tutorial resource use.

Snapshot now reads this object's ready/state fields from its validated UI span; context is Menu while a loaded tutorial is busy. Fixtures reproduce active40000000 versus terminal00100000 and prove correct object offsets. Native action requests cancel when tutorials open; the diagnostic route waits on this context instead of a guessed number of page clicks. Live verification pending.

Live update2026-10-14: state20000000 accepts confirmation; route now waits for that exact state. Pause/Back and tutorial transitions verified in1791932770890224300 and subsequent routes.1791964017664063800 moves beyond restaurant carriage with original shooting/reloads. Motion-QTE encounter was not reached in1791964760864676800; no live success is claimed.

## Knife stroke direction and mouse threshold (2026-10-18)

Owner report: V swung vertically, mouse swings were mirrored, and any 1-pixel motion with interact held started a swing. Live check (harness, frames 1 apart): the slash follows the processed
vector `owner+4+0xC` / `+0x10` as a SCREEN direction (x right, y down) with `(a,b)=(dx,dy)`; the old `(-dy,dx)` rotated every swing by 90 degrees (the earlier 'rightward = pi/2' reading of
80394548 was wrong). `hle_GameActionOwnerUpdate` now writes `(dx,dy)`. V is `kKnifeKeyDirection` = lower left to upper right (0.7071,-0.7071), verified: the slash starts at the lower left
and ends at the upper right. A mouse swing needs `kKnifeStrokeLength` = 40 raw counts accumulated in one stroke (pauses over 150 ms or releasing interact reset it), direction = accumulated vector.
Tests: test_input_bindings (`knife_key_direction_and_mouse_threshold`), test_game_input. The threshold is not configurable yet; the harness `motion` command did not start a swing in the live check (not investigated).
