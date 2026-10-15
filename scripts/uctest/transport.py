"""Two Windows named pipes (commands out, events in) carrying JSON lines."""
import time


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
