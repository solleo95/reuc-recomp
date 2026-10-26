# Automatic Multiplayer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Automatically host/join LAN or Internet games and start when both endpoints pass existing checks.

**Architecture:** A launcher NetworkSession owns direct and tunneled lobby candidates, selects a single winner, and handles port fallback before advertising. Existing Lobby readiness and game compatibility protocols remain intact.

**Tech Stack:** C++20, Winsock/adapter enumeration, ENet, ImGui/SDL, existing CTest and Python harness.

**Spec:** `docs/superpowers/specs/2026-10-26-auto-netplay-design.md`

## Global Constraints
- Default port 27888; fallback only for an unavailable TCP/UDP port.
- Host binds all IPv4 interfaces including localhost; client has one IP:Port input.
- No manual network-mode or readiness controls; preserve prerequisites and cancellation.
- No commits. Preserve pre-existing changes. Game tests use windowed harness and isolated saves.

## Review Focus
- Occupied TCP or UDP port: reserve another pair before publishing it (Task 1).
- Discovery failure: direct LAN still launches (Task 1).
- Competing connection paths: exactly one start handoff (Task 1).
- Invalid/empty endpoint and canceled connection: no launch or leaked listener (Tasks 1–2).
- Unsaved settings/calibration: auto-start waits until prerequisites pass (Task 2).

### Task 1: Automatic network session
Files: `common/netplay.{h,cpp}`, `launcher/nat_tunnel.{h,cpp}`, new `launcher/network_session.{h,cpp}`, `tests/test_network_session.cpp`, launcher/tests CMake files.
Interfaces: LobbyState reports listener availability and port conflicts. NetworkSession exposes state(), poll(bool ready), take_start(); state contains port, local addresses, Internet discovery and lobby state. Client address parser returns optional Endpoint.
- [x] Add failing tests for strict combined endpoint parsing, occupied TCP/UDP ports, LAN connection without STUN, prerequisite waiting and single handoff.
- [x] Implement listener status, typed bind conflicts and NetworkSession candidate ownership/fallback.
- [x] Run test_network_session, test_netplay, test_nat_tunnel and test_nat_mapping; add cancellation/competing-route cases.

### Task 2: Launcher workflow
Files: `launcher/main.cpp`, `launcher/translations.inc`, existing launcher smoke flow.
Consumes NetworkSession from Task 1; keeps router mapping lifecycle in launcher.
- [x] Update smoke expectations to automatic readiness, one client input, address copying and retained invalid-input/cancel coverage.
- [x] Replace host inputs/manual punch/readiness controls with two address rows; connect through one validated field. Keep first connected route and existing launch prerequisites.
- [x] Exercise native launcher smoke and inspect resulting host/connected captures.

### Task 3: Ship and verify
Files: build outputs and release manifests in existing distribution directories; no settings/saves changed.
- [x] Run full build/test stage, network history recovery harness, and focused network checks.
- [x] Review final task delta, resolve findings, rebuild affected targets if needed.
- [x] Update PC/laptop executables and maps with backups and matching hashes; update release manifests. Report any real-machine checks that remain.

### Follow-up: filtered LAN connection, five-second retry and last peer
User confirmed LAN target 192.168.50.137:27888 and requested a five-second wait before offering punch plus persistence of the last attempted IP:Port.
- [x] Compare both launcher logs: host listened while client direct TCP timed out; no gameplay started.
- [x] Read firewall state: Windows Private/Public firewall disabled, ESET running; no firewall changes. ESET UI access blocked by computer-use product policy, so the exact packet filter remains unconfirmed.
- [x] Add/test five-second client Lobby timeout and UDP fallback; keep incoming host wait unchanged.
- [x] Restore optional peer punch recovery, local endpoint selected from OS route, and endpoint/listener diagnostics.
- [x] Persist and reload canonical launcher.last_peer before connection attempt; smoke-test config roundtrip and clipboard/punch actions.
- [x] Run full tests and focused final review, then update both PCs with verified backups/hashes.
