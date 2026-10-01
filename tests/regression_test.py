#!/usr/bin/env python3
"""Regression tests for tinykv.

Usage:  python3 tests/regression_test.py ./build/kvserver

Each test starts the server in a fresh temporary directory (so it gets its
own empty kv.log), talks to it over TCP, and checks the replies.
"""
import os
import socket
import subprocess
import sys
import tempfile
import time

PORT = 9999
MAX_LINE = 512
MAX_KEY = 127


class Server:
    def __init__(self, binary, workdir):
        self.binary = binary
        self.workdir = workdir
        self.proc = None

    def start(self):
        self.proc = subprocess.Popen([self.binary], cwd=self.workdir,
                                     stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        deadline = time.time() + 5
        while time.time() < deadline:
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("server did not start")

    def kill(self):
        """Simulates a crash: SIGKILL, no chance to clean up."""
        self.proc.kill()
        self.proc.wait()


class Client:
    def __init__(self):
        self.sock = socket.create_connection(("127.0.0.1", PORT), timeout=3)
        self.buf = b""

    def send_raw(self, data):
        self.sock.sendall(data)

    def read_line(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("server closed the connection")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode()

    def cmd(self, line):
        self.send_raw(line.encode() + b"\n")
        return self.read_line()

    def close(self):
        self.sock.close()


def test_basic(srv):
    c = Client()
    assert c.cmd("SET user:1 alice smith") == "OK"
    assert c.cmd("GET user:1") == "alice smith"
    assert c.cmd("DEL user:1") == "OK"
    assert c.cmd("GET user:1") == "NOT_FOUND"
    assert c.cmd("DEL user:1") == "NOT_FOUND"
    assert c.cmd("FOO bar") == "ERROR"
    assert c.cmd("SET onlykey") == "ERROR"
    c.close()


def test_max_length_record_survives_restart(srv):
    """A maximum-length SET used to be truncated in the WAL (losing its
    '\\n'), which made replay stop there and silently drop every later write."""
    c = Client()
    assert c.cmd("SET before 1") == "OK"
    prefix = "SET big "
    value = "x" * (MAX_LINE - 1 - len(prefix))  # longest line the server accepts
    assert c.cmd(prefix + value) == "OK"
    assert c.cmd("SET after 2") == "OK"
    c.close()

    srv.kill()
    srv.start()

    c = Client()
    assert c.cmd("GET before") == "1"
    assert c.cmd("GET big") == value
    assert c.cmd("GET after") == "2"
    c.close()


def test_long_key_rejected(srv):
    """Keys longer than MAX_KEY used to be truncated, with the leftover bytes
    leaking into the value."""
    c = Client()
    ok_key = "k" * MAX_KEY
    long_key = "k" * (MAX_KEY + 1)
    assert c.cmd(f"SET {ok_key} v") == "OK"
    assert c.cmd(f"GET {ok_key}") == "v"
    assert c.cmd(f"SET {long_key} v") == "ERROR"
    assert c.cmd(f"GET {long_key}") == "ERROR"
    assert c.cmd(f"DEL {long_key}") == "ERROR"
    assert c.cmd(f"GET {ok_key}") == "v"  # untouched by the rejected SET
    c.close()


def test_pipelined_lines_across_recv_boundary(srv):
    """The buffer holds the start of one line; the next recv() delivers its
    end plus more commands. Their total exceeds MAX_LINE, but every single
    line is valid — this used to disconnect the client."""
    c = Client()
    value = "y" * 390
    c.send_raw(f"SET a {value}".encode())
    time.sleep(0.2)  # make sure the server consumes the partial line first
    c.send_raw(b"\n" + b"GET a\n" * 20)
    assert c.read_line() == "OK"
    for _ in range(20):
        assert c.read_line() == value
    c.close()


def test_oversized_line_disconnects(srv):
    c = Client()
    c.send_raw(b"SET a " + b"z" * (MAX_LINE * 2) + b"\n")
    try:
        c.read_line()
        raise AssertionError("expected the server to drop the connection")
    except (ConnectionError, ConnectionResetError):
        pass
    c.close()
    # ...and the server itself must still be alive
    c = Client()
    assert c.cmd("SET still alive") == "OK"
    c.close()


def test_crlf_line_endings(srv):
    """telnet and many clients end lines with CR LF; the CR must not end up
    in the stored value."""
    c = Client()
    c.send_raw(b"SET greeting hello world\r\n")
    assert c.read_line() == "OK"
    c.send_raw(b"GET greeting\r\n")
    assert c.read_line() == "hello world"
    assert c.cmd("GET greeting") == "hello world"  # plain LF still works
    c.close()


def test_delete_survives_restart(srv):
    c = Client()
    assert c.cmd("SET gone 1") == "OK"
    assert c.cmd("SET kept 2") == "OK"
    assert c.cmd("DEL gone") == "OK"
    c.close()
    srv.kill()
    srv.start()
    c = Client()
    assert c.cmd("GET gone") == "NOT_FOUND"
    assert c.cmd("GET kept") == "2"
    c.close()


def test_wal_failure_refuses_writes(srv):
    """If the WAL can't be written (here: kv.log -> /dev/full, every write
    fails with ENOSPC), a write must get ERROR -- never OK -- must not show up
    in memory, and reads must keep working."""
    if not os.path.exists("/dev/full"):
        print("      (skipped: no /dev/full on this system)")
        return
    srv.kill()
    log = os.path.join(srv.workdir, "kv.log")
    os.remove(log)
    os.symlink("/dev/full", log)
    srv.start()
    c = Client()
    assert c.cmd("SET a 1") == "ERROR"
    assert c.cmd("GET a") == "NOT_FOUND"   # not applied in memory either
    assert c.cmd("SET b 2") == "ERROR"     # stays read-only after a failure
    assert c.cmd("DEL a") == "NOT_FOUND"
    assert c.cmd("GET b") == "NOT_FOUND"
    c.close()


TESTS = [
    test_basic,
    test_max_length_record_survives_restart,
    test_long_key_rejected,
    test_pipelined_lines_across_recv_boundary,
    test_oversized_line_disconnects,
    test_crlf_line_endings,
    test_delete_survives_restart,
    test_wal_failure_refuses_writes,
]


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = os.path.abspath(sys.argv[1])
    failed = 0
    for test in TESTS:
        with tempfile.TemporaryDirectory() as workdir:
            srv = Server(binary, workdir)
            srv.start()
            try:
                test(srv)
                print(f"PASS  {test.__name__}")
            except Exception as e:  # noqa: BLE001
                failed += 1
                print(f"FAIL  {test.__name__}: {e!r}")
            finally:
                srv.kill()
    print(f"\n{len(TESTS) - failed}/{len(TESTS)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
