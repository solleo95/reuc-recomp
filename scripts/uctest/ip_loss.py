"""Actual loopback TCP/UDP drops via optional WinDivert 2.2 (Administrator).

Opening the filter can load WinDivert's signed driver. Only use with explicit
owner authorization; normal harness runs never import or open this filter.
"""
import ctypes
import heapq
import math
import random
import threading
import time
from pathlib import Path


def loss_filter(port, percent):
    if not isinstance(port, int) or not 1024 <= port <= 65535 or percent not in (1, 5):
        raise ValueError("require a test port and 1% or 5% loss")
    return (f"loopback and ip and ip.SrcAddr == 127.0.0.1 and ip.DstAddr == 127.0.0.1 "
            f"and ((tcp and (tcp.SrcPort == {port} or tcp.DstPort == {port})) "
            f"or (udp and (udp.SrcPort == {port} or udp.DstPort == {port})))")


def ip_payload_size(packet):
    if len(packet) < 20 or packet[0] >> 4 != 4:
        raise ValueError("expected IPv4 transport packet")
    if packet[9] == 6:
        return tcp_payload_size(packet)
    ip_size = (packet[0] & 15)*4
    total = int.from_bytes(packet[2:4], "big")
    if packet[9] != 17 or ip_size < 20 or total != len(packet) or ip_size+8 > total:
        raise ValueError("invalid IPv4 UDP length")
    udp_size = int.from_bytes(packet[ip_size+4:ip_size+6], "big")
    if udp_size < 8 or ip_size+udp_size != total:
        raise ValueError("invalid UDP length")
    return udp_size-8


def tcp_payload_size(packet):
    if len(packet) < 40 or packet[0] >> 4 != 4 or packet[9] != 6:
        raise ValueError("expected IPv4 TCP packet")
    ip_size = (packet[0] & 15)*4
    total = int.from_bytes(packet[2:4], "big")
    if ip_size < 20 or total != len(packet) or ip_size+20 > total:
        raise ValueError("invalid IPv4 length")
    tcp_size = (packet[ip_size+12] >> 4)*4
    if tcp_size < 20 or ip_size+tcp_size > total:
        raise ValueError("invalid TCP length")
    return total-ip_size-tcp_size


class PacketLoss:
    def __init__(self, dll_path, port, percent, delay=0, jitter=0):
        self.filter = loss_filter(port, percent)
        self.percent = percent
        if not all(math.isfinite(v) for v in (delay, jitter)) or not 0 <= jitter <= delay <= 5:
            raise ValueError("invalid IP delay/jitter")
        self._delay, self._jitter, self._port = delay, jitter, port
        self._dll = ctypes.WinDLL(str(Path(dll_path).resolve(strict=True)), use_last_error=True)
        signatures = {
            "WinDivertOpen": (ctypes.c_void_p, [ctypes.c_char_p, ctypes.c_int, ctypes.c_int16, ctypes.c_uint64]),
            "WinDivertRecv": (ctypes.c_int, [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint,
                                            ctypes.POINTER(ctypes.c_uint), ctypes.c_void_p]),
            "WinDivertSend": (ctypes.c_int, [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint,
                                            ctypes.POINTER(ctypes.c_uint), ctypes.c_void_p]),
            "WinDivertShutdown": (ctypes.c_int, [ctypes.c_void_p, ctypes.c_int]),
            "WinDivertClose": (ctypes.c_int, [ctypes.c_void_p]),
        }
        for name, (result, args) in signatures.items():
            function = getattr(self._dll, name)
            function.restype, function.argtypes = result, args
        self._stop = threading.Event()
        self._lock = threading.Condition()
        self._dropped = 0
        self._forwarded = 0
        self._udp_captured = 0
        self._udp_dropped = 0
        self._queue = []
        self._queued_bytes = 0
        self._error = None
        self._handle = self._dll.WinDivertOpen(self.filter.encode("ascii"), 0, 0, 0)
        if self._handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        self._thread = threading.Thread(target=self._run, name="netplay-IP-loss", daemon=True)
        self._sender = threading.Thread(target=self._send, name="netplay-IP-delay", daemon=True)
        try:
            self._sender.start()
            self._thread.start()
        except BaseException:
            self._stop.set()
            with self._lock:
                self._lock.notify_all()
            if self._sender.is_alive():
                self._sender.join(2)
            self._dll.WinDivertClose(self._handle)
            raise

    def _run(self):
        packet = ctypes.create_string_buffer(65575)
        address = (ctypes.c_uint64 * 10)()  # WINDIVERT_ADDRESS: 80 bytes, 8-byte alignment.
        length = ctypes.c_uint()
        rng = random.Random(12345)
        previous = [0.0, 0.0]
        sequence = 0
        try:
            while not self._stop.is_set():
                if not self._dll.WinDivertRecv(self._handle, packet, len(packet), ctypes.byref(length), address):
                    if self._stop.is_set():
                        return
                    raise ctypes.WinError(ctypes.get_last_error())
                data = packet.raw[:length.value]
                payload = ip_payload_size(data)
                udp = data[9] == 17
                ip_size = (data[0] & 15)*4
                direction = int.from_bytes(data[ip_size:ip_size+2], "big") == self._port
                # UDP is dropped on the wire; TCP ACKs receive the emulated RTT too.
                drop = payload > 0 and rng.random() < self.percent/100
                with self._lock:
                    due = time.monotonic()+self._delay+rng.uniform(-self._jitter, self._jitter)
                    if not udp:
                        due = max(previous[direction], due)
                    self._udp_captured += int(udp)
                    if drop:
                        self._dropped += 1
                        self._udp_dropped += int(udp)
                        continue
                    if self._queued_bytes+len(data) > 4*1024*1024:
                        raise RuntimeError("IP delay queue exceeded 4 MiB")
                    previous[direction] = due
                    sequence += 1
                    heapq.heappush(self._queue, (due, sequence, data, bytes(address)))
                    self._queued_bytes += len(data)
                    self._lock.notify_all()
        except Exception as error:
            self._fail(error)

    def _fail(self, error):
        with self._lock:
            if self._error is None:
                self._error = error
            self._stop.set()
            self._lock.notify_all()

    def _send(self):
        try:
            while True:
                with self._lock:
                    while not self._stop.is_set():
                        wait = self._queue[0][0]-time.monotonic() if self._queue else None
                        if wait is not None and wait <= 0:
                            break
                        self._lock.wait(wait)
                    if self._stop.is_set():
                        return
                    _, _, data, metadata = heapq.heappop(self._queue)
                    self._queued_bytes -= len(data)
                address = (ctypes.c_uint64 * 10).from_buffer_copy(metadata)
                sent = ctypes.c_uint()
                if not self._dll.WinDivertSend(self._handle, data, len(data), ctypes.byref(sent), address):
                    raise ctypes.WinError(ctypes.get_last_error())
                if sent.value != len(data):
                    raise RuntimeError("partial IP packet send")
                with self._lock:
                    self._forwarded += 1
        except Exception as error:
            self._fail(error)

    def stats(self):
        with self._lock:
            return {"actual_ip_packets_dropped": self._dropped, "ip_packets_delayed": self._forwarded,
                    "udp_packets_captured": self._udp_captured, "udp_packets_dropped": self._udp_dropped,
                    "filter_error": str(self._error) if self._error else None}

    def set_delay(self, delay, jitter=0):
        if not all(math.isfinite(v) for v in (delay, jitter)) or not 0 <= jitter <= delay <= 5:
            raise ValueError("invalid IP delay/jitter")
        with self._lock:
            self._delay, self._jitter = delay, jitter

    def check(self):
        with self._lock:
            error = self._error
        if error:
            raise RuntimeError("IP loss filter failed") from error

    def close(self):
        self._stop.set()
        with self._lock:
            self._lock.notify_all()
        shutdown_ok = self._dll.WinDivertShutdown(self._handle, 1)
        shutdown_error = ctypes.get_last_error()
        self._thread.join(2)
        self._sender.join(2)
        close_ok = self._dll.WinDivertClose(self._handle)
        close_error = ctypes.get_last_error()
        self._thread.join(1)
        if self._thread.is_alive() or self._sender.is_alive():
            raise RuntimeError("IP loss worker failed to stop")
        if not shutdown_ok:
            raise ctypes.WinError(shutdown_error)
        if not close_ok:
            raise ctypes.WinError(close_error)
        self.check()
