# Automatic multiplayer connection

The user approved simultaneous LAN/Internet hosting and automatic launch, and explicitly requested implementation without further approval checkpoints.

Host UI has no address, port, network-mode or readiness controls. Hosting first tries port 27888, then an available port, with TCP and UDP held before advertising it. Bind IPv4 on all interfaces, including loopback. Display Internet first and LAN second, each with a copyable IP:Port. A background discovery failure leaves LAN usable. Client accepts one validated IPv4:Port field (also localhost:Port). After connection and existing local prerequisites, the existing readiness/commit protocol starts both games automatically.

Use a direct TCP lobby for LAN/loopback and retain the existing ENet tunnel for Internet. Hosting offers both paths on the same port. Choose one connected lobby before making it ready; cancel the other. A direct handoff releases the tunnel's UDP port before the game binds it. A tunneled handoff keeps the tunnel and router lease alive through gameplay. Local address enumeration covers active IPv4 adapters; the LAN row can choose among their addresses. No additional dependency or rendezvous server is introduced. Internet access remains subject to the existing router/UPnP capabilities; an unavailable route gets an explicit status, not manual punch controls.

Alternative considered: force all traffic through the existing tunnel. Rejected because local play must work directly without NAT setup. Separate LAN/Internet modes were explicitly rejected by the user.

Preserve save isolation, compatibility checks, CPU calibration, startup shader warmup behavior and cancellation. The separately fixed rollback backpressure must ship on both machines with identical executables.

Follow-up requested by user: TCP connect gets five seconds, then the client automatically tries UDP at the same endpoint and offers punch recovery. The optional host recovery section takes the client's endpoint; host binding remains automatic. The last attempted canonical IP:Port is saved in launcher.last_peer and restored on startup/reload. Private/loopback punch addresses use the OS-selected local source address; Internet addresses retain STUN discovery. Neither firewall settings nor security rules are changed.
