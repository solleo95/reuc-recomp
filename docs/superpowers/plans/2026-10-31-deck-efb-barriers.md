# EFB copy final barriers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the unused transfer-source transition of the final EFB-copy mip without changing rendered pixels, then measure on Steam Deck.

**Architecture:** Keep the blits and the inter-mip dependencies unchanged. Finish the earlier source mips and the final destination mip in one dependency command, each with its actual old layout. The existing Vulkan smoke forwards intercepted calls to the real driver and checks that only mips actually used as blit sources enter the source layout.

**Tech Stack:** C++20, Vulkan synchronization2, existing GPU smokes and Python 3.13 uctest.

**Spec:** `docs/superpowers/specs/2026-10-30-performance-roadmap-design.md`, Phase 1 item 7; owner selected autonomous continuation on 2026-10-31 after the EFB candidate report.

## Global Constraints

- Preserve pixels, copy order, quality, render scale and clocks. Vulkan-only change; D3D paths already transition on demand.
- Work in the existing feature/linux-steamdeck checkout to preserve Claude's uncommitted base. No commits or pushes.
- All game runs use uctest, isolated saves and a 1280x720 window. Check the Deck is idle before each run; clean test binaries and folders afterward.
- Compare the same deterministic demo first, then confirm live FPS with the owner's settings and defaults. Keep a correct small saving only when no regression is demonstrated.

## Review Focus

- One mip: no transfer-source transition; all copied pixels still readable.
- Several mips: earlier levels retain their blit dependencies, final level is readable; existing sampled-mip pixel checks pass.
- Scene restore rewrites a sampled texture: existing upscale and effects smokes remain identical.
- Recycled copy textures: existing retirement and repeated-copy tests still pass.
- Remote run fails: restore the staged game and clean only this experiment's files.

## Task 1: Exact EFB copy barrier reduction

**Files:** Modify `renderer/vulkan_renderer.cpp` and `tests/renderer_smoke.cpp`; record results in `docs/spikes/P1-efb-copies.md`.

**Interfaces:** `VulkanRenderer::record_texture_copy` keeps its signature. Test-only Vulkan wrappers forward all work to the real driver.

- [x] Add forwarding test wrappers recording the last inter-image blit's destination and how many of its mip levels enter transfer-source layout. For the existing three-mip EFB copy assert 2 levels; for the existing one-mip copy assert 0. Build and run renderer_smoke: RED because the old implementation transitions 3 and 1 respectively.
- [x] Replace the last-mip intermediate barrier and whole-chain final barrier with one dependency command containing a final-mip destination-to-shader barrier plus, when present, a preceding-mips source-to-shader barrier. Keep transfer read/write source access and fragment sampled-read destination access.
- [x] Build and run renderer_smoke, renderer_upscale_smoke and relevant effects smokes. Run the full build/test command before completion; record every failure.
- [x] Save baseline binaries first, cross-build Linux, compare deterministic opening replay with owner effects and defaults (ledger ruling: shorter than fight4); check matching guest work and compare GPU category cost. Use short live perf runs to confirm FPS/pacing; repeat only if noise prevents the decision.
- [x] Keep or remove the experiment based on correctness and measurements. If kept, deploy through the existing staging procedure, clean experiment files, record evidence and obtain final independent review. Leave changes uncommitted.
