"""Reproduce Windows' independent TCP/UDP ephemeral port allocation collision."""
import socket
import subprocess
import sys
import unittest


def check(executable):
    with socket.socket() as tcp:
        tcp.bind(("127.0.0.1", 0))
        target = tcp.getsockname()[1]
    for _ in range(65536):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.bind(("0.0.0.0", 0))
            if udp.getsockname()[1] == target:
                break
    else:
        raise unittest.SkipTest("TCP/UDP ephemeral ranges do not share the selected port")
    child = subprocess.Popen([executable, "--bridge"], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    try:
        # --bridge has bounded waits; failure exits before publishing its local endpoints.
        line = child.stdout.readline()
        assert line.strip(), "Tunnel could not connect with aligned TCP/UDP allocators"
        host_port, client_port = map(int, line.split())
        assert host_port != client_port
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as game:
            game.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            game.bind(("127.0.0.1", host_port))
    finally:
        _, error = child.communicate("quit\n", timeout=15)
        if error:
            print(error, file=sys.stderr)
    assert child.returncode == 0, child.returncode


if __name__ == "__main__":
    try:
        check(sys.argv[1])
    except unittest.SkipTest as error:
        print(error)
        sys.exit(77)
