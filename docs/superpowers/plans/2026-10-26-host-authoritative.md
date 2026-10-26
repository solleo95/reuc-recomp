# Host-authoritative cooperative play

User authorized implementation without further checkpoints.

## Contract

Only the host runs the guest world, owns saves, and renders game graphics. The client displays compressed host frames, plays host audio, and sends player-two input. Client rendering, decoding, or delivery acknowledgements never gate host simulation. The existing launcher and LAN/NAT endpoint selection remain the entry point.

Use native Windows JPEG codecs for independent 1280x720-or-smaller frames, initially at up to 60 Hz. Independent frames permit discarding obsolete work without decoder recovery. PCM audio is stereo S16 32 kHz. This trades bandwidth for a small, dependable implementation without new runtime dependencies; interframe compression is a future bandwidth optimization.

Bound every queue. Encoding, decoding and socket I/O run off the game/render threads. GPU readback is asynchronous and skipped when busy. Send only one unacknowledged video frame; acknowledge reception independently of decoding. Input and audio have bounded queues and explicit validation. Disconnect cancels workers and reports a useful error. Client saves and guest state are never initialized.

## Work

1. Add tested bounded remote session, native JPEG encoding/decoding, versioned messages, input/control/audio lanes.
2. Add nonblocking renderer readback, including scaling and completion lifetime handling.
3. Replace active rollback session with host input routing. Add thin SDL client startup before guest initialization, audio output and harness support.
4. Verify malformed messages, cancellation, slow decoder/backpressure, player-two input, and host progress with paired isolated harness processes. Run full build/tests and final review.
5. Stage and deploy verified executables/manifests to local dist-raw/dist-release and laptop dist-raw, preserving settings/saves.

## Acceptance

Client logs zero guest simulation and skips game shader warmup. Host progresses while client rendering is intentionally delayed. Both see the same host scene and second-player actions reach host. All tests are windowed, muted, isolated through the harness. Audible quality remains a manual verification because automated runs are muted.

## Frame pacing follow-up

Use the ordinary host GX queue. Immediate detached GX completion let the guest run
repeated polling/clear iterations and starved loading threads; an experimental VI
selection path was removed after the live startup regression. Service networking
after each visible host frame. GPU capture deadlines retain their phase rather than
drifting after each late poll. The client buffers two to three unique images and
adapts to measured arrival rate; a50ms one-image fallback prevents starvation for a
source below4FPS. Client frames.csv records actual unique presents.

The ordered-queue paired run at
`build/autotest/remote-play-1792992640561172200/report.json` passed: normal host/client
45.06FPS and simulation ratio1.000; delayed client4.99FPS, host43.68FPS and ratio1.001.
Both runs verified P2 ammunition15→14 without changing P1; client guest steps remained0.
Final adaptive-cadence live verification and deployment follow the final build.

Final review found HDR native overlays missing from the SDR stream and a below4FPS
buffer starvation case. Both were corrected; targeted review found no further P1/P2.
The final CPU suite (`build/host-stream-final-paced-build.log`) passed200/200, including
overlay alpha composition/validation, QTE epochs, network cancellation, and cadence.
The HDR-specific GPU readback path needs an HDR10 display for a live check; ordinary
presentation/readback has a separate focused GPU smoke. The full historical GPU smoke
still contains unrelated six-slot assumptions while the renderer uses sixteen slots.
