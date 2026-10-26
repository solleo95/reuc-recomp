"""Bounded TCP/UDP delay relay; TCP loss stalls, UDP loss drops datagrams."""

import collections
import heapq
import math
import random
import selectors
import socket
import threading
import time


class DelayRelay:
    """One TCP connection and UDP peer; delays are seconds per direction."""

    def __init__(self, target_port, port=0, seed=12345):
        self._listener = socket.socket()
        self._udp = None
        try:
            self._udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self._listener.bind(("127.0.0.1", port))
            self._listener.listen(1)
            self._listener.setblocking(False)
            self.port = self._listener.getsockname()[1]
            self._udp.bind(("127.0.0.1", self.port))
            self._udp.setblocking(False)
        except Exception:
            self._listener.close()
            if self._udp is not None:
                self._udp.close()
            raise
        self._target_port = target_port
        self._random = random.Random(seed)
        self._udp_random = random.Random(seed ^ 0x554450)
        self._lock = threading.Lock()
        self._profile = (0.0, 0.0, 0.0, 0.0)
        self._bandwidth = 0
        self._stats = {"segments": 0, "retransmissions": 0, "bytes": 0,
                       "udp_datagrams": 0, "udp_dropped": 0, "udp_bytes": 0}
        self._stop = threading.Event()
        self._error = None
        self._thread = threading.Thread(target=self._run, name="netplay-delay-relay")
        self._udp_thread = threading.Thread(target=self._run_udp, name="netplay-udp-delay-relay")
        started = []
        try:
            for worker in (self._thread, self._udp_thread):
                worker.start()
                started.append(worker)
        except Exception:
            self._stop.set()
            for worker in started:
                worker.join(3)
            self._listener.close()
            self._udp.close()
            raise

    def set_profile(self, delay, jitter=0.0, *, loss_rate=0.0, retransmit_delay=0.0):
        if (not all(math.isfinite(v) for v in (delay, jitter, loss_rate, retransmit_delay))
                or not 0 <= jitter <= delay <= 5 or not 0 <= loss_rate <= 1
                or not 0 <= retransmit_delay <= 5 or (loss_rate and not retransmit_delay)):
            raise ValueError("invalid bounded delay/retransmission profile")
        with self._lock:
            self._profile = (delay, jitter, loss_rate, retransmit_delay)

    def set_bandwidth(self, bytes_per_second):
        """TCP only: each direction serializes at this rate (0 = unlimited) behind a 128 KiB queue."""
        if not math.isfinite(bytes_per_second) or bytes_per_second < 0:
            raise ValueError("invalid bandwidth")
        with self._lock:
            self._bandwidth = bytes_per_second

    def stats(self):
        with self._lock:
            return dict(self._stats)

    def check(self):
        with self._lock:
            error = self._error
        if error is not None:
            raise RuntimeError("delay relay failed") from error

    def close(self):
        self._stop.set()
        for worker in (self._thread, self._udp_thread):
            worker.join(3)
        if self._thread.is_alive() or self._udp_thread.is_alive():
            raise RuntimeError("delay relay failed to stop")
        self.check()

    def _run_udp(self):
        host = ("127.0.0.1", self._target_port)
        client = None
        queue = []
        queued_size = sequence = 0
        selector = selectors.DefaultSelector()
        try:
            selector.register(self._udp, selectors.EVENT_READ)
            while not self._stop.is_set():
                now = time.monotonic()
                for _ in range(64):
                    if not queue or queue[0][0] > now:
                        break
                    _, _, destination, data = queue[0]
                    try:
                        self._udp.sendto(data, destination)
                    except BlockingIOError:
                        break
                    heapq.heappop(queue)
                    queued_size -= len(data) + 64
                timeout = min(0.02, max(0.001, queue[0][0] - now)) if queue else 0.02
                if not selector.select(timeout):
                    continue
                for _ in range(64):
                    try:
                        data, source = self._udp.recvfrom(65536)
                    except BlockingIOError:
                        break
                    except ConnectionResetError:
                        continue
                    if source == host:
                        if client is None:
                            continue
                        destination = client
                    elif client is None:
                        client = source
                        destination = host
                    elif source == client:
                        destination = host
                    else:
                        continue
                    with self._lock:
                        delay, jitter, loss_rate, _ = self._profile
                        self._stats["udp_datagrams"] += 1
                        self._stats["udp_bytes"] += len(data)
                    arrival = time.monotonic() + delay + self._udp_random.uniform(-jitter, jitter)
                    lost = self._udp_random.random() < loss_rate
                    # Account for heap entries too, bounding empty datagram floods.
                    if lost or queued_size + len(data) + 64 > 4 * 1024 * 1024:
                        with self._lock:
                            self._stats["udp_dropped"] += 1
                        continue
                    sequence += 1
                    heapq.heappush(queue, (arrival, sequence, destination, data))
                    queued_size += len(data) + 64
        except Exception as exc:
            with self._lock:
                self._error = exc
        finally:
            selector.close()
            self._udp.close()

    def _run(self):
        peers = []
        selector = selectors.DefaultSelector()
        try:
            selector.register(self._listener, selectors.EVENT_READ)
            while not self._stop.is_set() and not peers:
                if selector.select(0.02):
                    client, _ = self._listener.accept()
                    peers.append(client)
                    # Bounded connect also makes cancellation bounded.
                    host = socket.create_connection(("127.0.0.1", self._target_port), 1)
                    peers.append(host)
            selector.unregister(self._listener)
            if len(peers) != 2:
                return
            queues = [collections.deque(), collections.deque()]
            sizes = [0, 0]
            eof = [False, False]
            shut = [False, False]
            link_free = [0.0, 0.0]
            for peer in peers:
                peer.setblocking(False)
                peer.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            while not self._stop.is_set():
                now = time.monotonic()
                for i, peer in enumerate(peers):
                    # queue i contains bytes destined for peer i.
                    if eof[1 - i] and not queues[i] and not shut[i]:
                        peer.shutdown(socket.SHUT_WR)
                        shut[i] = True
                    events = 0
                    with self._lock:
                        capacity = 128 * 1024 if self._bandwidth else 1024 * 1024
                    if not eof[i] and sizes[1 - i] < capacity:
                        events |= selectors.EVENT_READ
                    if queues[i] and queues[i][0][0] <= now:
                        events |= selectors.EVENT_WRITE
                    try:
                        selector.unregister(peer)
                    except KeyError:
                        pass
                    if events:
                        selector.register(peer, events, i)
                if all(eof) and not any(queues):
                    return
                due = [q[0][0] for q in queues if q and q[0][0] > now]
                timeout = min(0.02, max(0, min(due) - now)) if due else 0.02
                if not selector.get_map():  # Windows select rejects empty sets while delayed queues drain.
                    self._stop.wait(timeout)
                    continue
                for key, mask in selector.select(timeout):
                    i = key.data
                    if mask & selectors.EVENT_READ:
                        try:
                            data = peers[i].recv(max(1, min(65536, capacity - sizes[1 - i])))
                        except BlockingIOError:
                            continue
                        if not data:
                            eof[i] = True
                        else:
                            with self._lock:
                                delay, jitter, loss_rate, retransmit_delay = self._profile
                                bandwidth = self._bandwidth
                            q = queues[1 - i]
                            # Model lost segments recovering later: TCP preserves every byte and
                            # holds subsequent segments behind the missing one. No actual IP drop.
                            segments = retransmissions = 0
                            for offset in range(0, len(data), 1200):
                                segment = memoryview(data)[offset:offset+1200]
                                arrival = time.monotonic() + delay + self._random.uniform(-jitter, jitter)
                                lost = bool(loss_rate and self._random.random() < loss_rate)
                                arrival += retransmit_delay if lost else 0
                                if bandwidth:
                                    link_free[1 - i] = max(time.monotonic(), link_free[1 - i]) + len(segment) / bandwidth
                                    arrival = max(arrival, link_free[1 - i] + delay)
                                arrival = max(arrival, q[-1][0] if q else arrival)
                                q.append((arrival, segment))
                                segments += 1
                                retransmissions += lost
                            with self._lock:
                                self._stats["segments"] += segments
                                self._stats["retransmissions"] += retransmissions
                                self._stats["bytes"] += len(data)
                            sizes[1 - i] += len(data)
                    if mask & selectors.EVENT_WRITE:
                        arrival, data = queues[i][0]
                        try:
                            sent = peers[i].send(data)
                        except BlockingIOError:
                            continue
                        sizes[i] -= sent
                        if sent == len(data):
                            queues[i].popleft()
                        else:
                            queues[i][0] = (arrival, data[sent:])
        except (ConnectionResetError, ConnectionAbortedError, BrokenPipeError):
            # Peer quit terminates the stream, including queued writes.
            return
        except Exception as exc:
            with self._lock:
                self._error = exc
        finally:
            selector.close()
            for peer in peers:
                peer.close()
            self._listener.close()


def selftest():
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(3)
    failures = []

    def echo():
        try:
            with listener.accept()[0] as peer:
                while True:
                    data = peer.recv(65536)
                    if not data:
                        return
                    peer.sendall(data)
        except Exception as exc:
            failures.append(exc)

    worker = threading.Thread(target=echo)
    worker.start()
    relay = DelayRelay(listener.getsockname()[1])
    try:
        relay.set_profile(0.025, 0.005)
        with socket.create_connection(("127.0.0.1", relay.port), 2) as client:
            client.settimeout(3)
            payload = bytes(range(256)) * 2048
            started = time.monotonic()
            client.sendall(payload)
            received = bytearray()
            while len(received) < len(payload):
                part = client.recv(65536)
                assert part, "unexpected EOF"
                received.extend(part)
            assert received == payload, "stream reordered or truncated"
            assert time.monotonic() - started >= 0.04, "roundtrip delay absent"
            relay.set_profile(.005, loss_rate=1, retransmit_delay=.08)
            started = time.monotonic()
            client.sendall(b"recovered TCP data")
            recovered = bytearray()
            while len(recovered) < len(b"recovered TCP data"):
                part = client.recv(64)
                assert part, "unexpected EOF during recovery"
                recovered.extend(part)
            assert recovered == b"recovered TCP data", "recovery model corrupted the stream"
            assert time.monotonic()-started >= .16, "retransmission delay absent"
            assert relay.stats()["retransmissions"] >= 2
            relay.set_profile(0)
            relay.set_bandwidth(400000)   # 100 KB through one direction takes 0.25 s; the echo overlaps the way back.
            started = time.monotonic()
            client.sendall(payload[:100000])
            echoed = 0
            while echoed < 100000:
                part = client.recv(65536)
                assert part, "unexpected EOF under bandwidth limit"
                echoed += len(part)
            assert time.monotonic() - started >= .22, "bandwidth limit absent"
    finally:
        relay.close()
        worker.join(3)
        listener.close()
    assert not worker.is_alive(), "echo worker leaked"
    assert not failures, failures
    idle = DelayRelay(1)
    idle.close()
    assert not idle._thread.is_alive(), "idle relay leaked"
    with socket.socket() as closing_listener:
        closing_listener.bind(("127.0.0.1", 0))
        closing_listener.listen(1)
        closing_listener.settimeout(3)
        closing_relay = DelayRelay(closing_listener.getsockname()[1])
        try:
            closing_relay.set_profile(.1)
            with socket.create_connection(("127.0.0.1", closing_relay.port), 2) as client, closing_listener.accept()[0] as host:
                client.settimeout(3)
                host.settimeout(3)
                # Both readers reach EOF while delayed writes still need draining.
                client.sendall(b"client")
                host.sendall(b"host")
                client.shutdown(socket.SHUT_WR)
                host.shutdown(socket.SHUT_WR)
                assert client.makefile("rb").read() == b"host"
                assert host.makefile("rb").read() == b"client"
            closing_relay._thread.join(2)
            assert not closing_relay._thread.is_alive(), "delayed EOF did not drain"
            closing_relay.check()
        finally:
            closing_relay.close()
    reset_listener = socket.socket()
    reset_listener.bind(("127.0.0.1", 0))
    reset_listener.listen(1)
    reset_listener.settimeout(3)
    reset_relay = DelayRelay(reset_listener.getsockname()[1])
    reset_client = socket.create_connection(("127.0.0.1", reset_relay.port), 2)
    reset_host = reset_listener.accept()[0]
    try:
        import struct

        # Windows uses unsigned shorts; POSIX uses ints for SO_LINGER.
        import sys

        linger = struct.pack("HH" if sys.platform == "win32" else "ii", 1, 0)
        reset_host.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, linger)
        reset_host.close()
        reset_relay._thread.join(2)
        assert not reset_relay._thread.is_alive(), "peer reset not observed"
        reset_relay.check()
    finally:
        reset_client.close()
        reset_host.close()
        reset_relay.close()
        reset_listener.close()
    active_listener = socket.socket()
    active_listener.bind(("127.0.0.1", 0))
    active_listener.listen(1)
    active_listener.settimeout(3)
    active_relay = DelayRelay(active_listener.getsockname()[1])
    active_client = socket.create_connection(("127.0.0.1", active_relay.port), 2)
    active_host = active_listener.accept()[0]
    try:
        active_relay.set_profile(1, 0)
        active_client.sendall(b"pending")
        active_relay.close()
        assert not active_relay._thread.is_alive(), "active relay leaked"
    finally:
        active_client.close()
        active_host.close()
        active_relay.close()
        active_listener.close()
    unavailable = socket.socket()
    unavailable.bind(("127.0.0.1", 0))
    unavailable_port = unavailable.getsockname()[1]
    unavailable.close()
    failed_relay = DelayRelay(unavailable_port)
    failed_client = socket.create_connection(("127.0.0.1", failed_relay.port), 2)
    try:
        failed_relay._thread.join(2)
        try:
            failed_relay.close()
        except RuntimeError as exc:
            assert isinstance(exc.__cause__, (ConnectionRefusedError, TimeoutError)), exc
        else:
            raise AssertionError("connect failure hidden")
    finally:
        failed_client.close()
    print("relay selftest: delay/order/cancellation OK")


if __name__ == "__main__":
    selftest()
