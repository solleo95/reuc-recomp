
"""Run session-migrate with a Windows-compatible, no-overwrite file writer."""

import os
import sys
import tempfile
from pathlib import Path

from session_migrate import conversion
from session_migrate.errors import JsonlError


def write_windows(path: Path, data: bytes) -> tuple[int, int]:
    """Write a new target file on Windows, without overwriting an existing one."""
    path = Path(os.path.abspath(Path(path).expanduser()))

    if os.path.lexists(path):
        raise JsonlError(f"refusing to overwrite existing target: {path}")

    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        descriptor, temporary_name = tempfile.mkstemp(
            prefix="." + path.name + ".",
            suffix=".tmp",
            dir=path.parent,
        )
        temporary_path = Path(temporary_name)

        try:
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())

            # On Windows, rename fails if the destination already exists.
            # The temporary file is in the same directory as the destination.
            os.rename(temporary_path, path)
            info = path.stat()
            return info.st_dev, info.st_ino
        finally:
            temporary_path.unlink(missing_ok=True)
    except OSError as exc:
        raise JsonlError(
            f"cannot write target session {path}: {exc.strerror or exc}"
        ) from exc


if os.name == "nt":
    conversion.write_private_atomic = write_windows

from session_migrate.cli import main

if __name__ == "__main__":
    sys.exit(main())
