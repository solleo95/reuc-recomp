# I3 — port-owned PC prompt boundaries, RBUE08

2026-10-14. Investigation only; no game-image edits or global atlas substitution.

## Confirmed resource structure

Data/layout/game.arc, dialog.arc and Data/script/s00c1.arc are U8 containers (55AA382D). Named ACT layouts in game.arc include A/B/AB/C/Z/CZ Press, A/B/C/Z Repeat, R_Hshake/N_Hshake, Normal and Success variants. These names identify candidate scopes; they do not establish a native semantic action on their own.

ACT_A_Press.brlyt and ACT_B_Press.brlyt are RLYT version8. RootPane640×480, Center, child1P/2P/All groups. Picture panes tex1/tex2/tex are120×120 at local(0,−90); group1P(−160,40),2P(160,40),All(0,40). word_1P/2P/All containers are at(0,−140). Texture names Abutton.tpl/Bbutton.tpl. R_Hshake uses cont.tpl; N_Hshake uses nunchaku.tpl. Geometry/name/size parsed from actual bounded pane sections, not inferred from screenshots.

Runtime pane addresses, global matrices/alpha and lifetime are not verified yet. BRLYT offsets cannot be reused as runtime object offsets. Unknown scopes retain original graphics.

Live1791974064031852100 confirms the older Picture copy8038BC60 is used by actual game menu/HUD despite the signature database's HomeButton library label.62885 calls,128 bounded object records,0 invalid spans. Observed Center,TITLE,logo,rights,chapter/character/weapon panes,bullet_1P,knife_tex_1P,Life,Damage. Caller8038A2B8 is the original Pane::Draw virtual dispatch. Name+0xB8 and alpha+0xB5 agree with original FindPane/DrawSelf disassembly and live values. Newer803CFA84 has different offsets(nameB4/alphaCE) and is not assumed interchangeable. Layout owner/root/matrix and destructor/reuse gates remain open; no suppression is enabled.

## SDK candidates remain gated

identified_sdk.txt contains nw4hbm::lyt::Picture::DrawSelf at8038BC60/803CFA84 and Layout::Build8038AEB0, plus other HomeButton library methods. Names are signature candidates from a separate SDK copy; do not hook them as game nw4r methods without actual argument/call evidence.80366E14 is hle_ResMeshDraw in the generated table and is used by scene-pass diagnostics for3D; it is not evidence of a UI Picture boundary. Actual game Picture/Pane chain remains to identify.

## QTE producer evidence

Player event handler800676A0 lives in the player vtable (entry803F23C0). It calls event object's virtual getter at+0xC and dispatches table803F2368. Type7→80067728 writes actor+630=1 at800677A0; type10→800677F4 writes2 at8006786C. Gate800F51F4 returns max(0,float(scene+6BEC)−float(scene+6C00)−float(scene+6C14)); semantic name unproven.800F40C8 selectors2/4 gate the corresponding handler and update scene6BD4/6BD8.

Several unrelated classes return7/10; those constants alone do not identify the actual producer. A bounded readonly entry observer for800676A0 records the actual actor/event/vtable/getter/caller and mode; it keeps the original AOT body and UC_TRACE behavior. No instructions are decoded/executed by the probe, no extra game callbacks and no state writes. Actor/event spans validated; event metadata is capped64 unique vtable/mode pairs. Live encounter still pending.

Live1791970599119260500: event vtable803F7644/getter800D9EA4, delivery caller8003FEC8. Original getter returns6 (damage). Adjacent803F7634/getter800D9E9C returns7, a concrete producer-family candidate. No type7/10 observed in that route. Entry observer test confirms disabled behavior, readonly registers/memory, invalid-span tolerance and64-record cap.

Stage3 Task1 pure description model is implemented/tested: all aliases/modifiers preserved, unavailable logical dependencies omitted, cleared actions empty, heldKnife and bareShake typed, grouped64×64 source aliases avoid cartesian expansion. Renderer and original-icon suppression are not enabled yet.

## Verified SDK owner/lifetime boundary and native implementation

Live1791975381484577600 and1791978223378120600: Layout::Build8038AEB0 is called from80047CAC (LR80047CB0), with the original resource filename preserved inr29. Root pane is stored atLayout+0x10. The same Layout address92EDBFB8 is reused across AS000/title/ZAB; registration must be replaced on every Build, including unsupported layouts.

Layout destructor8038AD94 releases ancillary+0x14 first, then root+0x10 and owned lists. Unregister at entry, before either release. Pane base destructor80389B20 unlinks/destroys children; Picture destructor8038B948 delegates to it. Parent+0x0C and name+0xB8 verified through Build/FindPane; list child links arepane+4, headpane+0x14. Root destruction also removes ownership.

Picture8038BC60 calls DrawQuad80394038 at8038BD18 (LR8038BD1C), preserving pictureThis inr28. Original DrawQuad delegates to80393AD8 and completes a single four-vertex quad. The port uses scoped UC_ENTER guards only for Build, Layout/Pane destruction and this exact DrawQuad caller. Original AOT bodies and all SDK state changes still execute; no HLE continuation, instruction decoding, game writes or extra callbacks are introduced. A batch/producer checkpoint can annotate only the one completed quad; intervening submissions/extra geometry fail closed.

PromptRegistry validates owner/root/ancestry/material spans, cycles and address reuse. Semantic profiles are exact ACT_A/B Press/Repeat and ACT_R_Hshake; unknown layouts/panes preserve original drawing. AB/C/Z/Nunchuk semantics remain gated. Final-present batches can replace a proven quad with immutable prompt metadata; nonpresent/texture-copy batches retain original geometry. First two native routes1791978223378120600 and1791978533637037400 passed R/G/repeatingKnife and29–31 gameplay presentations/sec, but saw no ACT annotations. This is not live prompt acceptance. Bounded pane→layout observation is being added to find actual menu/footer/tutorial owners.

Native output overlay is implemented through the existing pinned ImGui Vulkan backend, private context, system SegoeUI and vector mouse/keycap/gesture graphics. It runs after EFB scaling, before present/readback. GPU fixtures cover alternatives/cleared/held/chords, alpha0, rotated/cropped panes,1280/4K/8K outputs and unchanged EFB readback. A CPU text-clipping-before-transform bug was exposed and corrected by the rotated fixture. Output font density scales with output size; latest4K/8K fixture changed3869/13322 pixels. Multi-slot/pending-shutdown checks and scoped review remain in progress.

Diagnostic formatter regression fixed separately: integer arithmetic RR forms use rT,rA; floating/paired arithmetic and comparison operands use f registers. ISA translation/runtime semantics were unchanged; test_format passed.

## Next proof gates

- Identify actual game Layout/ResourceAccessor/Picture boundaries and runtime layout name, pane matrix/alpha and destructor lifetime.
- Confirm menu/tutorial/ACT scopes separately; A maps to Confirm/Interact according to verified owner context, B to Back/Fire. AB and motion-QTE must retain original timing/failure and correct input context; do not infer context from a texture alone.
- Compose host-owned icons/labels into final output-size slot after EFB scaling, before present/readback; existing ImGui dependency can use its official Vulkan backend. Bindings, clear state, alternatives and logical combos feed the same model as launcher.
- Add registry/model/GPU lifecycle/alpha/resize/capture tests before enabling scoped original-icon suppression. Region/language profiles remain explicitly bounded to verified images.

## Menu/tutorial prompts are font glyphs (2026-10-14, after Codex→Claude migration)

mes_*.txtres (UTF-8) tutorial strings format Wii buttons through "%s%s" with glyph strings 37220..37232: ★ Α Β Γ Ζ Δ Θ Ξ Σ Τ Ω; menu footer strings "Α%s"/"Ξ%s". Decoding the matching cells of sysres/font0/utf8_{D,E,F,G,I,J,S}.brfnt shows Α=A, Β=B, Γ=C, Ζ=Z, Δ=Control Pad, Θ=Control Stick, Ξ=minus, Σ=%, Τ=arrow, Ω=∞. The Russian patch re-maps Cyrillic onto Latin-1/extended code points, not Greek, so U+0391..U+03A9 reach the font only as button glyphs.

Boundary: CharWriter::Print(u16) 803B7CF0 (old homebutton-lib copy used by the game), called only from TextWriterBase<char>/<wchar_t> at 803BA234/803BC134 (LR 803BA238/803BC138). It fetches the glyph via font vtable+0x50 and calls PrintGlyph 803B7E20, which emits exactly one GXBegin(0x80,0,4). The scoped Print guard maps Α→confirm/interact, Β→back/fire, Δ→weapon previous/next (tutorial-busy counts as gameplay); the one quad is replaced by a compact port prompt (primary binding, icon-only mouse/wheel, "—" when unassigned). Γ Ζ Θ Ξ remain original and are logged "[prompt-glyph] … covered 0". Display-list recording or any extra/missing quad keeps the original glyph.
