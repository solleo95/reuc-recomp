"""Check that the game window appears only after shader warmup, without system input."""
import ctypes
from ctypes import wintypes
import threading

from .client import Game
from . import steps
from .overlay import read_png


def visible_game_window(pid):
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.IsWindowVisible.argtypes = [wintypes.HWND]
    user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    found = []

    @callback_type
    def visit(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            name, title = ctypes.create_unicode_buffer(256), ctypes.create_unicode_buffer(256)
            user32.GetClassNameW(hwnd, name, len(name))
            user32.GetWindowTextW(hwnd, title, len(title))
            if name.value != "UcCompileScreen" and title.value == "Resident Evil: The Umbrella Chronicles":
                found.append(hwnd)
        return True

    if not user32.EnumWindows(visit, 0):
        raise ctypes.WinError(ctypes.get_last_error())
    return bool(found)


def main():
    stopped = threading.Event()
    failures = []
    watcher = None

    def observe(pid, log_path):
        try:
            while not stopped.wait(0.01):
                if visible_game_window(pid):
                    log = log_path.read_text(errors="replace")
                    if "[startup] graphics ready" not in log:
                        failures.append("game window became visible before graphics were ready")
                        return
        except Exception as error:
            failures.append(f"window observation failed: {error}")

    class ObservedGame(Game):
        def _launch(self):
            nonlocal watcher
            super()._launch()
            watcher = threading.Thread(target=observe, args=(self.process.pid, self.run_dir / "run.log"))
            watcher.start()

    game = ObservedGame(speed=8, game_args=["--render-scale", "1"])
    try:
        with game:
            steps.skip_movies(game)
            game.advance(120)  # The title state precedes its fade-in animation.
            assert visible_game_window(game.process.pid), "ready game window is hidden"
            capture = game.capture()
            width, height, channels, rows = read_png(capture)
            lit = sum(max(row[x:x + 3]) > 16 for row in rows for x in range(0, len(row), channels))
            assert lit > width * height // 100, "title frame is black"
            log = game.finish()
            stopped.set()
            watcher.join()
            assert game.process.returncode == 0, "game did not exit cleanly"
            assert "[startup] graphics ready" in log, "graphics initialization did not finish"
            assert not failures, "; ".join(failures)
            print(f"Window startup passed; capture: {capture}")
    finally:
        stopped.set()
        if watcher:
            watcher.join()


if __name__ == "__main__":
    main()
