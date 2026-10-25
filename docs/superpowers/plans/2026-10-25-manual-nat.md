# Manual NAT punching implementation plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** After an unsuccessful connection, show the client's public IPv4:port for the host's manual “Punch NAT” action, matching projectz without a rendezvous server.

**Architecture:** Keep a UDP tunnel alive in the launcher through lobby and gameplay. STUN discovery, punching and reliable/unreliable peer traffic share its socket. Existing TCP lobby/control and UDP gameplay protocols are bridged through loopback. ENet supplies reliable ordered delivery and fragmentation; gameplay datagrams use a separate unreliable channel. The launcher remains alive for both players until the game exits.

**Tech stack:** C++20, Winsock, ENet, existing ImGui launcher and CTest.

**Spec:** User request and projectz sources (`crates/transport/src/nat.rs`, `crates/host/src/lib.rs`, `crates/app/src/ui/connect.rs`).

## Constraints and decisions

- Preserve existing direct command-line game networking and unrelated working-tree changes.
- Use public STUN only for endpoint discovery; peers exchange addresses themselves.
- Keep the UDP socket and mapping alive across launcher/game handoff.
- UI stays responsive; one worker owns transport state, commands and snapshots are synchronized.
- Cancellation closes sockets promptly. Failed discovery must never advertise a guessed public port.
- Symmetric NAT and blocked UDP can still require forwarding or VPN.
- No commits or install into the real game directory requested.

## Review focus

- Malformed or spoofed STUN responses: match source, cookie, transaction and bounded attributes.
- Retry/cancel: retain the client port; stop old workers before replacement.
- Lobby/game transition: preserve UDP mapping, TCP EOF ordering and stream boundaries.
- Backpressure and packet loss: bounded queues, reliable control and independent datagrams.
- Local forwarding: bind loopback only and refuse malformed endpoint input.

## Tasks

- [x] 1. Add bounded STUN codec and persistent ENet tunnel. Test malformed responses, endpoint validation, cancellation, reliable packets, datagrams and two consecutive local TCP sessions. Run failing tests first, then implement and rerun.
- [x] 2. Wire the launcher: connection timeout guidance, public endpoint copy, host punch input, retry/cancel, Russian/English text and launcher lifetime. Exercise actual ImGui controls with the existing smoke harness and add tunnel handoff checks.
- [x] 3. Build with `build.bat --no-install`, run CTest and check two real game instances through the tunnel. Final independent review follows below.

## Verification ledger

- Baseline: test_nat_mapping, test_netplay, test_launcher_locale, test_launch: 4/4 passed.
- Environment: sandbox process startup fails on locked BitLocker G:; approved unsandboxed execution works. Editor read unavailable; use RTK read fallback.
- RED: `test_nat_tunnel` failed with `NAT tunnel not implemented`; `test_options` rejected `--net-loopback` before implementation.
- GREEN: fake NAT/STUN fixture rejects bad transactions/lengths, blocks host ingress until manual punch, drops the first subsequent connection packet, then carries the lobby, a new game stream, bulk reliable packets and repeated input datagrams on unchanged ports.
- UI smoke: initial failure guidance, cancel, typing peer address, punch action, both-ready handshake and exactly-once game handoff passed. Captures: `build/nat-ui/network-nat-help.bmp`, `network-nat-punch.bmp`, `network-connected.bmp`. A clipped Cancel at 720 px was fixed and rechecked.
- Full build: `build.bat --no-install`, 185/185 CTests passed; dist staged, no copy into installed game. Log: `build/nat-full-build.log`.
- Live STUN: `build/tests/test_nat_tunnel.exe --stun` returned mapped, symmetric=false, no error.
- Live gameplay: `py -3.13 build/nat-game-check.py` passed via the harness (1280x720, muted, unlocked FPS, isolated saves). Both players fired, reloaded and used knife; bridge exited cleanly. Report: `build/autotest/nat-game-1792948262266522800/report.json`.
- Ruling: retain existing public IP:port open-join semantics. Core review suggested authentication, but existing TCP lobby accepts any peer too; the tunnel exposes only its own fixed game port. A new shared-secret UX exceeds this task. People knowing the host endpoint can attempt to join, as before.
- Real two-provider CGNAT traversal remains unverified; symmetric NAT and blocked UDP remain limitations.
- Final review (GPT-6-Astra): two P2 findings, no other important defects. Manual IP:port typing normalized before editing finished; independent TCP/UDP allocators could select colliding ports.
- Final: fixed typed endpoint — actual ImGui character-entry smoke RED (`typed IP:port was corrupted`), parser moved to Connect/Create.
- Final: fixed port collision — `test_nat_ports.py` aligned Windows allocators and failed against `test_nat_tunnel-before-ports.exe`; same test passed against fixed executable. Both host and client reserve UDP+TCP pair; occupied TCP candidates retry.
- Final verification after fixes: `build.bat --no-install` passed 186/186 tests (including allocator-alignment regression), staged dist, no installation copy. Updated ImGui smoke passed per-character IP:port entry and the complete NAT/readiness flow. `git diff --check` clean. Final review findings resolved; no deferred minor findings.
