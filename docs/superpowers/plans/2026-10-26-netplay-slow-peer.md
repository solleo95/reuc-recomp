# Slow Peer Recovery Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans inline. User already authorized fixes without further agreement. No commits.

**Goal:** Prevent slow peers and rollback from exhausting input history, and keep both simulations progressing together.

**Architecture:** Reuse the existing work estimate carried in each immutable input and the checkpointed network timestep. Bound replay input lead and ordinary peer prediction independently of the two-second recovery history. Preserve startup, save isolation and packet validation.

**Tech Stack:** C++20, existing rollback/CTest and Python windowed harness.

**Evidence/spec:** PC and laptop logs from 2026-10-26 02:48 UTC, under dist-raw/logs and build/lan-desync-20261026/laptop. Confirmed hashes agree; laptop advances about60 of120 required steps/sec. Host waits in120-frame bursts; client rejects inputs too far ahead, host exhausts sampling history. LAN ingress remains unanswered until host sends to client; no security-policy changes in scope.

## Constraints and review focus
- Keep current checkout and unrelated work. Preserve configs/saves on deployment.
- Same canonical input pair must produce the same timestep during forward play and replay.
- Work estimate changes must invalidate predictions; work timings exclude explicit network/wall pacing sleeps.
- A long replay stall must retain pending input edges without unbounded queue growth.
- Brief peer pacing must not flash the waiting window; real outages retain cancellation and timeout.
- All game checks use harness, 1280x720, fps0, isolated saves. No fullscreen.

### Task 1: Bounded input and peer lead
- [x] Add failing NetInputClock long-replay tests, keeping existing short-replay edge preservation assertions.
- [x] Bound replay sampling to eight slots beyond the forward input horizon; discard elapsed sampling debt when full, retain unconsumed native input.
- [x] Add forward peer pacing using remote input progress plus configured delay and eight prediction frames; show waiting only after250ms. Preserve history confirmation and15s no-progress timeout.
- [x] Run frame tests and delayed-input pair harness.

### Task 2: Adaptive deterministic timestep
- [x] Add failing tests for symmetric bounded work-derived period and changed-work prediction invalidation.
- [x] Populate existing NetFrame.work_ns from forward step cost, filtered with existing median estimator. Derive checkpointed game period from max canonical work with10% headroom, rounded up to1ms, bounded120..20Hz.
- [x] Use the same period for replay input clock. Verify long/short rollback and unequal workloads with matching hashes in the harness.

### Task 3: Verify and deliver
- [x] Full build/CTest, focused final review; address actionable findings with regression evidence.
- [x] Update PC dist-raw/dist-release and laptop dist-raw with backups and hash verification.
- [x] Report LAN filtering evidence and remaining real-machine checks accurately.
