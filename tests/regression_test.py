#!/usr/bin/env python3
"""Regression tests for tinykv.

Usage:  python3 tests/regression_test.py ./build/kvserver

Each test starts the server in a fresh temporary directory (so it gets its
own empty kv.log) on a free port, talks to it over TCP, and checks the
replies. If the server was built with sanitizers, any sanitizer report in
its stderr fails the test.
"""
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import zlib

PORT = None  # chosen per test
MAX_LINE = 512
MAX_KEY = 127


SANITIZER_MARKERS = ("ERROR: AddressSanitizer", "WARNING: ThreadSanitizer",
                     "runtime error:", "ERROR: LeakSanitizer")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Server:
    def __init__(self, binary, workdir):
        self.binary = binary
        self.workdir = workdir
        self.proc = None
        self.args = []
        self.stderr_path = os.path.join(workdir, "server.stderr")

    def start(self, *args):
        """Starts the server; extra command-line args are remembered for restarts."""
        global PORT
        if args:
            self.args = list(args)
        PORT = free_port()
        self.stderr = open(self.stderr_path, "a")
        self.proc = subprocess.Popen([self.binary, "-p", str(PORT)] + self.args,
                                     cwd=self.workdir,
                                     stdout=subprocess.DEVNULL,
                                     stderr=self.stderr)
        deadline = time.time() + 5
        while time.time() < deadline:
            if self.proc.poll() is not None:
                self.stderr.close()
                raise RuntimeError(f"server exited with code {self.proc.returncode}")
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("server did not start")

    def stop(self, sig=signal.SIGTERM, timeout=10):
        """Graceful shutdown; returns the exit code."""
        self.proc.send_signal(sig)
        code = self.proc.wait(timeout=timeout)
        self.stderr.close()
        return code

    def stderr_text(self):
        with open(self.stderr_path, errors="replace") as f:
            return f.read()

    def threads(self):
        with open(f"/proc/{self.proc.pid}/status") as f:
            for line in f:
                if line.startswith("Threads:"):
                    return int(line.split()[1])
        return -1

    def kill(self):
        """Simulates a crash: SIGKILL, no chance to clean up."""
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        if not self.stderr.closed:
            self.stderr.close()

    def sanitizer_report(self):
        with open(self.stderr_path, errors="replace") as f:
            text = f.read()
        return text if any(m in text for m in SANITIZER_MARKERS) else None


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


def wal_record(payload):
    """A WAL record exactly as the server writes it: CRC32 + payload."""
    return b"%08x %s\n" % (zlib.crc32(payload), payload)


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


def test_crash_under_concurrent_load(srv):
    """8 clients write concurrently; the server is SIGKILLed mid-stream.
    After restart, every write that was acknowledged with OK must be there
    (writes still in flight may or may not have made it -- both are fine)."""
    clients, stop = 8, threading.Event()
    acked = [0] * clients        # highest i acknowledged for client c

    def writer(c):
        try:
            cl = Client()
            i = 0
            while not stop.is_set():
                i += 1
                if cl.cmd(f"SET c{c}:{i} v{i}") != "OK":
                    return
                acked[c] = i
        except (OSError, ConnectionError):
            pass                 # the server was killed under us

    threads = [threading.Thread(target=writer, args=(c,)) for c in range(clients)]
    for t in threads:
        t.start()
    time.sleep(1.0)
    srv.kill()
    stop.set()
    for t in threads:
        t.join()
    assert sum(acked) > 0, "no writes were acknowledged"

    srv.start()
    c = Client()
    for client, last in enumerate(acked):
        for i in range(1, last + 1):
            assert c.cmd(f"GET c{client}:{i}") == f"v{i}", f"lost acked write c{client}:{i}"
    c.close()
    print(f"      {sum(acked)} acknowledged writes, all present after the crash")

def test_pipelined_write_then_read(srv):
    """With the event loop, a write is completed asynchronously; commands
    pipelined after it must still run after it, in order."""
    c = Client()
    c.send_raw(b"SET a 1\nGET a\nSET a 2\nGET a\nDEL a\nGET a\nDEL a\n")
    assert [c.read_line() for _ in range(7)] == \
        ["OK", "1", "OK", "2", "OK", "NOT_FOUND", "NOT_FOUND"]
    c.close()


def test_half_close_gets_all_replies(srv):
    """`printf 'SET..\\nGET..\\n' | nc` style: the client sends everything,
    shuts down its sending side, and must still receive every reply."""
    s = socket.create_connection(("127.0.0.1", PORT), timeout=3)
    s.sendall(b"".join(b"SET k%d v%d\n" % (i, i) for i in range(50)) + b"GET k49\n")
    s.shutdown(socket.SHUT_WR)
    data = b""
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break                                  # server closed after replying
        data += chunk
    s.close()
    assert data == b"OK\n" * 50 + b"v49\n"


def test_many_connections_few_threads(srv):
    """300 concurrent connections are served by a handful of threads."""
    srv.kill()
    srv.start("--threads", "2")
    clients = [Client() for _ in range(300)]
    for i, c in enumerate(clients):
        c.send_raw(b"SET c%d %d\n" % (i, i))
    for c in clients:
        assert c.read_line() == "OK"
    for i, c in enumerate(clients):
        assert c.cmd(f"GET c{i}") == str(i)
    threads = srv.threads()
    print(f"      300 connections, server has {threads} threads")
    # main + 2 workers + commit thread (+1 helper thread under ThreadSanitizer)
    assert threads <= 5
    for c in clients:
        c.close()


def test_connection_limit(srv):
    srv.kill()
    srv.start("--max-connections", "5")
    time.sleep(0.2)                                # let the startup probe connection close
    clients = [Client() for _ in range(5)]
    for c in clients:
        assert c.cmd("GET x") == "NOT_FOUND"
    extra = Client()
    assert extra.read_line() == "ERROR too many connections"
    try:
        extra.read_line()
        raise AssertionError("expected the 6th connection to be closed")
    except (ConnectionError, ConnectionResetError):
        pass
    clients[0].close()
    time.sleep(0.2)
    again = Client()                               # a slot is free again
    assert again.cmd("GET x") == "NOT_FOUND"
    for c in clients[1:] + [again]:
        c.close()


def test_graceful_shutdown(srv):
    """SIGTERM (what `docker stop` sends) and SIGINT (Ctrl+C): every write
    that was acknowledged is on disk, and the process exits with code 0."""
    for sig in (signal.SIGTERM, signal.SIGINT):
        c = Client()
        for i in range(100):
            assert c.cmd(f"SET g{i} {sig.name}") == "OK"
        idle = Client()                            # an idle connection must not block shutdown
        assert srv.stop(sig) == 0
        assert "shutting down" not in srv.stderr_text()   # goes to stdout, not an error
        c.close()
        idle.close()
        srv.start()
        c = Client()
        assert c.cmd("GET g99") == sig.name
        c.close()


def test_graceful_shutdown_under_load(srv):
    clients, stop = 8, threading.Event()
    acked = [0] * clients

    def writer(c):
        try:
            cl = Client()
            i = 0
            while not stop.is_set():
                i += 1
                if cl.cmd(f"SET s{c}:{i} v") != "OK":
                    return
                acked[c] = i
        except (OSError, ConnectionError):
            pass

    threads = [threading.Thread(target=writer, args=(c,)) for c in range(clients)]
    for t in threads:
        t.start()
    time.sleep(0.5)
    assert srv.stop(signal.SIGTERM) == 0
    stop.set()
    for t in threads:
        t.join()
    srv.start()
    c = Client()
    for client, last in enumerate(acked):
        for i in range(1, last + 1, 7):            # sample: every 7th, plus the last
            assert c.cmd(f"GET s{client}:{i}") == "v", f"lost s{client}:{i}"
        if last:
            assert c.cmd(f"GET s{client}:{last}") == "v"
    c.close()
    print(f"      {sum(acked)} acknowledged writes before SIGTERM, all present")


def test_compaction_bounds_the_log(srv):
    srv.kill()
    srv.start("--compact-bytes", "8192")
    c = Client()
    for i in range(3000):
        assert c.cmd(f"SET key{i % 25} value-{i}") == "OK"
    c.close()
    log = os.path.join(srv.workdir, "kv.log")
    snap = log + ".snap"
    size = os.path.getsize(log)
    print(f"      3000 writes: kv.log is {size} bytes, kv.log.snap {os.path.getsize(snap)} bytes")
    assert size < 8192 + 1024
    assert os.path.exists(snap)
    srv.kill()                                     # crash, then recover from snapshot + log
    srv.start()
    c = Client()
    for k in range(25):
        last = max(i for i in range(3000) if i % 25 == k)
        assert c.cmd(f"GET key{k}") == f"value-{last}"
    c.close()


def test_corrupt_log_refuses_start_until_repair(srv):
    srv.kill()
    log = os.path.join(srv.workdir, "kv.log")
    good = wal_record(b"SET a 1")
    bad = bytearray(wal_record(b"SET b 2"))
    bad[-2] = ord("9")                             # damage: CRC no longer matches
    with open(log, "wb") as f:
        f.write(good + bytes(bad) + wal_record(b"SET c 3"))
    try:
        srv.start()
        raise AssertionError("server started on a corrupt log")
    except RuntimeError as e:
        assert "exited" in str(e)
    assert "--repair" in srv.stderr_text()         # tells the operator what to do
    assert os.path.getsize(log) == len(good) + len(bad) + len(wal_record(b"SET c 3"))

    srv.start("--repair")
    c = Client()
    assert c.cmd("GET a") == "1"
    assert c.cmd("GET b") == "NOT_FOUND"
    assert c.cmd("GET c") == "NOT_FOUND"
    c.close()
    assert os.path.getsize(log) == len(good)


TESTS = [
    test_basic,
    test_max_length_record_survives_restart,
    test_long_key_rejected,
    test_pipelined_lines_across_recv_boundary,
    test_oversized_line_disconnects,
    test_crlf_line_endings,
    test_delete_survives_restart,
    test_wal_failure_refuses_writes,
    test_crash_under_concurrent_load,
    test_pipelined_write_then_read,
    test_half_close_gets_all_replies,
    test_many_connections_few_threads,
    test_connection_limit,
    test_graceful_shutdown,
    test_graceful_shutdown_under_load,
    test_compaction_bounds_the_log,
    test_corrupt_log_refuses_start_until_repair,
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
                srv.kill()
                report = srv.sanitizer_report()
                if report:
                    raise AssertionError("sanitizer report:\n" + report)
                print(f"PASS  {test.__name__}")
            except Exception as e:  # noqa: BLE001
                if srv.proc and srv.proc.poll() is None:
                    srv.kill()
                failed += 1
                print(f"FAIL  {test.__name__}: {e!r}")
    print(f"\n{len(TESTS) - failed}/{len(TESTS)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
