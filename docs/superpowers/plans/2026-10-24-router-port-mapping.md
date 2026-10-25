# Automatic router port mapping

Scope approved: home-router port forwarding only; CGNAT/relay deferred.

1. Add a launcher-only UPnP mapping owner, using pinned MiniUPnPc 2.3.3.
   Map the selected TCP and UDP port to the local interface chosen by discovery.
   Check existing rules, verify ownership before renewal/deletion, roll back partial failure.
2. Discover/map/renew on a background worker. Preserve mappings across lobby/game handoff;
   hide the launcher while its game runs. Cancel without blocking the UI. Clean up on exit.
   Prefer 10-minute leases renewed every 3 minutes; support permanent-only routers with
   explicit status and documented crash-cleanup limitation.
3. Show discovery/result/external endpoint in the lobby. Failures preserve LAN/manual access.
   Keep smoke tests isolated from the real router.
4. Test ownership, partial failures, cancellation, renewal, unavailable routers and lifecycle.
   Build, run native launcher smoke, document real-router verification limits.

No changes to rollback, game simulation or the wire protocol.

Completed 2026-10-24: implementation, 184-test build, native launcher smoke and
real-router TCP/UDP create/renew/delete probe passed. Added transient renewal retry
without tearing down existing mappings. External peer reachability remains untested.
Build staged in dist; owner's installed game directory was not updated by this task.
