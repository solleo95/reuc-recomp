"""Two Windows named pipes (commands out, events in) carrying JSON lines; SocketTransport: the same over two local TCP ports (a game on the Deck);
UnixSocketTransport: the same over the game's two Unix-domain sockets (a game on this Mac, game/test_pipe_posix.cpp)."""
import os
import socket
import time


class SocketTransport:
    def __init__(self, cmd_port, evt_port, connect_seconds=20.0, alive=None):
        deadline = time.monotonic() + connect_seconds
        self._cmd = self._connect(cmd_port, deadline, alive)
        self._evt = self._connect(evt_port, deadline, alive)
        self._lines = self._evt.makefile("rb")

    @staticmethod
    def _connect(port, deadline, alive):
        while True:
            try:
                sock = socket.create_connection(("127.0.0.1", port), timeout=2)
                sock.settimeout(None)
                return sock
            except OSError:
                if time.monotonic() >= deadline or (alive is not None and not alive()):
                    raise TimeoutError(f"test socket on port {port} not available")
                time.sleep(0.05)

    def send_line(self, line):
        self._cmd.sendall(line.encode("utf-8") + b"\n")

    def read_line(self):
        try:
            data = self._lines.readline()
        except OSError:
            return None
        return data.decode("utf-8").rstrip("\r\n") if data else None

    def close(self):
        for handle in (self._cmd, self._evt):
            try:
                handle.close()
            except OSError:
                pass


class UnixSocketTransport(SocketTransport):
    def __init__(self, name, connect_seconds=20.0, alive=None):
        base = (os.environ.get("XDG_RUNTIME_DIR") or "/tmp") + f"/uc-test-{name}"   # the game's socket_path()
        deadline = time.monotonic() + connect_seconds
        self._cmd = self._connect_unix(base + "-cmd.sock", deadline, alive)
        self._evt = self._connect_unix(base + "-evt.sock", deadline, alive)
        self._lines = self._evt.makefile("rb")

    @staticmethod
    def _connect_unix(path, deadline, alive):
        while True:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                sock.connect(path)
                return sock
            except OSError:
                sock.close()
                if time.monotonic() >= deadline or (alive is not None and not alive()):
                    raise TimeoutError(f"test socket {path} not available")
                time.sleep(0.05)


class PipeTransport:
    def __init__(self, name, connect_seconds=20.0, alive=None):
        self.name = name
        deadline = time.monotonic() + connect_seconds
        self._cmd = self._open(rf"\\.\pipe\uc-test-{name}-cmd", "wb", deadline, alive)
        self._evt = self._open(rf"\\.\pipe\uc-test-{name}-evt", "rb", deadline, alive)

    def _open(self, path, mode, deadline, alive):
        while True:
            try:
                return open(path, mode, buffering=0)
            except (FileNotFoundError, PermissionError):
                if time.monotonic() >= deadline or (alive is not None and not alive()):
                    raise TimeoutError(f"test pipe uc-test-{self.name} not available")
                time.sleep(0.05)

    def send_line(self, line):
        self._cmd.write(line.encode("utf-8") + b"\n")

    def read_line(self):
        try:
            data = self._evt.readline()
        except OSError:
            return None
        return data.decode("utf-8").rstrip("\r\n") if data else None

    def close(self):
        for handle in (self._cmd, self._evt):
            try:
                handle.close()
            except OSError:
                pass
