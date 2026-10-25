# Direct-IP cooperative play

The launcher owns a two-player lobby: create, join by IPv4 address and port,
cancel, and toggle Ready. Both connected and ready launches each local game once.
The games wait at a startup barrier before guest entry. Host owns P1 and menu
navigation; client owns P2. Each machine renders and simulates the original coop.

Use reliable TCP and deterministic input lockstep, reusing the virtual clock and
scheduler. Independent bounded send/receive workers pipeline raw pad samples and
native actions. Both peers apply sample N at N+32; compare historical guest-state
digests when their matching input arrives. Maintain independent action queues and
contexts per player. Realtime gameplay never waits for a network packet: exhausted
input/transport buffers, mismatch, disconnect or timeout end the session visibly.
Only startup and explicitly accelerated harness runs may wait for packets. Align
startup epochs using an RTT probe. Pace both peers with the same accumulated
schedule and historical CPU/render work pair, excluding network waits. Apply that
shared period at the existing game-frame wait so lower render FPS does not imply
slow-motion; preserve KPAD's original guest-thread scheduling. Filter isolated
work spikes and reuse existing XXH64 for full-memory checks. Harness consumption means
the delayed input reached both canonical pads, so scripts cannot race ahead.

An authoritative state-replication implementation would require identifying all
game state and correcting it safely. Video streaming would require a separate
capture/codec stack. Input lockstep fits the existing deterministic runtime.

Both peers require the same build and game data. Before guest entry the host sends
a bounded save snapshot to a fresh client session directory; the client's personal
saves remain untouched. Remove session storage after SaveFs closes, including failed
connections. Refuse incompatible peers or malformed snapshots with a clear error.
Disable mods and synchronize simulation settings for network games; local graphics
can differ. No mid-mission join, reconnect, matchmaking, NAT traversal or rollback.

Verification: unit tests for framing, validation, ready/start transition and two
independent pads; launcher launch-argument tests; two real harness processes with
isolated saves/config, muted 1280x720 windows and fps-limit 0. Exercise host/join,
both-ready start, original two-player activation, distinct aim/fire/reload, state
agreement and disconnect. Preserve single-player tests. No commits or pushes.
Also exercise RTT40/100/200ms with jitter on a TCP relay, record simulation speed,
frame gaps and observed shot latency, and verify different personal client saves
remain byte-identical after a session using the host snapshot.
