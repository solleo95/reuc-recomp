# Native PC prompts — этап3

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans. Native исполнение согласовано владельцем; шаги RED/GREEN, без автоматических коммитов.

**Goal:** заменить проверенные Wii-подсказки иконками текущих назначений мыши/клавиатуры на стороне порта.

**Architecture:** чистая модель описывает альтернативы и logical combinations через существующие Bindings. Scoped SDK bridge связывает только доказанные layout/picture instances с semantic prompt и записывает immutable requests в соответствующий frame. Native Vulkan UI композируется в output-size slot после EFB scaling, до present/readback; игровые файлы сохраняются неизменными.

**Tech Stack:** C++20, SDL3, имеющийся Dear ImGui1.92.9b с официальным Vulkan backend, Vulkan1.3, существующий AOT/xref и readonly probes. Новых библиотек нет.

**Spec:** `../specs/2026-10-13-native-pc-controls-design.md`, §§2,3,5. Исходные доказательства `../../spikes/I3-prompt-boundaries.md`.

## Global Constraints

- Никакого редактирования game/DATA или глобальной замены атласов. Unknown layout/owner/material сохраняет оригинальный draw.
- SDK runtime offsets не берутся из BRLYT resource offsets. Каждый entry/continuation/field/span/lifetime сначала подтверждается.
- Контекст Menu/Gameplay/QTE — проверенное состояние игры; draw timing/FPS/texture alone его не определяют.
- Bindings из того же runtime snapshot/config, что управление; пустые списки действительно выключают подсказку действия и показывают «Не назначено».
- Полный список альтернатив и logical combinations, включая отдельно очищаемые Interact+Fire, Interact+Motion, bareQTE motion. Зависимости пустого logical action не выдаются за рабочее сочетание.
- Host GPU/ImGui resources принадлежат renderer thread; guest SDK bridge пишет только frame metadata под baton. Не передаёт живые pane pointers renderer thread.
- Snapshot принадлежит frame, а не произвольному следующему presentation; destructor/reuse/reset исключает stale prompt.
- Native/noCPUISA interpreter/JIT/device models. Коммиты только по просьбе; планы/пользовательские файлы не удаляются. Ghidra не устанавливать без реальной необходимости и согласия.
- Visible/audible/skipintro diagnostics, copied saves/config, process-local input, NoActivate. Сборкаdist, установка/E, сохранение userconfig/saves.

## Review Focus

- Cleared/remapped dependency of a logical combo: Task1 resolves unavailable combos correctly.
- Same-named tex/RootPane in unrelated layouts and allocator address reuse: Task2 owner/lifetime fixtures, fallback draw.
- Rotated/scaled/faded/cropped panes and resize between submission/readback: Tasks2/3 matrix/alpha/extent fixtures.
- Multiple GPU frames, font update and shutdown while in flight: Task3 validation/fence/resource checks.
- A+B button-QTE versus Grenade chord and motion-QTE: Task2 original consumer/state proof and Task4 live window tests; no guessed context switch.

## Task1: чистая модель подсказки

**Files:** Create `common/input_prompts.h/.cpp`, `tests/test_input_prompts.cpp`; Modify common/tests CMake.

**Interfaces:** `PromptChoice {vector<Binding> bindings; bool held;}`; `PromptAlternative {vector<PromptChoice> choices; optional<PromptGesture> gesture;}`; `PromptDescription {Action action; vector<PromptAlternative> alternatives;}`; enum `PromptGesture {directional_motion,shake}`. `describe_prompt(const Bindings&,Action)` validates input and returns the complete ordered alternatives. Physical bindings stay typed; renderer turns keys/mouse/wheel into host labels/icons. No cartesian explosion: a logical chord uses two choice groups (Interact held, Fire press), each containing the source action's alternatives.

- [ ] RED defaults/cleared/multiple/modifiers/wheel: description matches all assignments and stable order.
- [ ] RED Grenade special follows rebound Interact/Fire; absent dependency omits that alternative while retainingG. KnifeGesture depends on Interact; V retained independently. Clear all removes physical and special entries.
- [ ] RED qte_motion independent of modifier, clearedQTE empty; invalid action/kind/duplicate input fails without mutation.
- [ ] Implement through existing validate_bindings; no SDL/JSON/game-memory dependency. GREEN targeted test.

## Task2: доказанная scoped SDK граница

**Files:** Create `game/prompt_bridge.h/.cpp`, `tests/test_prompt_bridge.cpp`; Modify `game/entry_probe.h/.cpp`, `game/CMakeLists.txt`, `project/uc.cfg`, `project/hle_map.txt`, `runtime/hle/gx.h/.cpp`, `renderer/gx_packets.h`; Update I3.

**Consumes:** Task1 model; actual ACT resources/pane geometry from I3; original AOT functions, validated gx::Memory and packet ownership.

**Produces:** verified `PromptHookProfile` table (layout/resource/pane identity + exact native entry/continuation + object field offsets/spans); `PromptDraw {PromptDescription description; array<float,8> output_corners; float alpha;}` stored by value in `FramePacket::prompts`. Registry registration/removal/reset is under guest baton, never renderer dereference of guest memory.

- [ ] First identify actual game ResourceAccessor/Layout.Build/Picture.DrawSelf and destructor through bounded readonly entry probes/xref. HomeButton signature candidates are not production addresses. Record one concrete table with caller/constructor/argument/span evidence inI3.
- [ ] Verify menu/footer, tutorial and ACT scopes separately. Reuse existing game_input_context only where proven; inspect button-QTE state/consumer before mappingAB. Keep original timers/failure; if unsupportedC/Z/motion dependency is found, record and resolve the native action boundary before claiming covered.
- [ ] RED profile matching: allowlisted layout+pane/material matches; identical pane name in other layout staysoriginal; bad spans/unknown resource preserve draw. RED destructor/address reuse and frame snapshot retain correct owner/generation.
- [ ] RED world matrix/alpha/source crop→output corners, bounds/finite validation; no source resource writes. Confirm both modern prompt geometry and original draw suppression occur in the same frame.
- [ ] Add only confirmed SDK-level replacement/continuation toHLE/cfg. Original animation/layout logic remains AOT; suppress only the registered original icon draw. Save immutable prompt into current native frame. GREEN focused bridge/input tests.

## Task3: Vulkan слой в разрешении вывода

**Files:** Create `renderer/prompt_overlay.h/.cpp`; Modify `renderer/vulkan_renderer.h/.cpp`, `renderer/CMakeLists.txt`, `launcher/CMakeLists.txt`, `tests/renderer_smoke.cpp`.

**Interfaces:** `PromptOverlay::initialize(VkInstance,VkPhysicalDevice,VkDevice,VkQueue,uint32_t family,uint32_t frame_slots)`, `record(VkCommandBuffer, span<const PromptDraw>, RenderExtent output)`, `shutdown()`. Uses private ImGui context and existing staticcore plus official imgui_impl_vulkan.cpp; RGBA8 slot target, dynamicrendering, buffer count matched to existing two frame slots. `record` is renderer-owner-only and has no platform input backend.

- [ ] RED smoke: synthetic LMB/RMB/W/S/wheel/chord/empty prompts on transparent output; pixel/readback check; no prompt changes EFB texture copies or original scene pixels outside overlay.
- [ ] Initialize backend from existing Vulkan device/queue with RGBA8 dynamic-rendering info; host keyboard labels from SDL, explicit fallback for reserved codes. Segoe UI loaded from system (no font redistributed). Mouse/key/wheel/gesture icons drawn as native vector primitives and text; no generated raster dependency.
- [ ] Record after EFB→slot blit and before slot→present blit. Correct TRANSFER_DST↔COLOR_ATTACHMENT↔TRANSFER_SRC barriers, LOAD/store, private font descriptor lifetime. Never draw into EFB/GPU copies used by the game.
- [ ] Preserve corner transform and alpha; output-size glyph quality independent of internal scale. Captures read composed slot; renderer resize/fullscreen/manualscale follow existing path.
- [ ] GREEN GPUvalidation: empty/transparent/multiple/cleared, rotated/faded panes,1280/4K/8K, resize, recycle/font updates, pending fences/shutdown. Repeat existing renderer fixtures only where touched.

## Task4: реальные экраны и завершение

- [ ] Visible/audible/skipintro autonomous route: save dialog/menu/footer, tutorial pages, train interaction and real QTE. Compare original icon removal + host labels against currentconfig; repeat cleared/rebound profiles. No fixed texture-name-only semantic guessing.
- [ ] Actual AB and motion-QTE success/failure windows; state and original acceptance prove correct actions. Live motion/DPI calibration from stage1 remains a required gate, not hidden by the new artwork.
- [ ] Fullbuild.bat/CTest/relevantGPU fixtures; dist/S /E. All uncovered UI and emulation-like remnants recorded separately. Asset hashes unchanged.
- [ ] Fresh strongest-model final whole-plan review after required tasks complete; fix material findings and repeat affected checks. No automatic commit/push.

## Coverage / status

Spec§5→Tasks1–4; all alternatives/clear/logical dependencies→Task1; original lifecycle/context/AB→Task2; output/present/capture→Task3; live bindings/QTE→Task4. Task2 gates production suppression until actual SDK identity is verified. Investigation addresses and artifacts inI3; scoped model/render work can proceed independently of that gate.
