#!/usr/bin/env python3
"""What do N open connections cost the server?

Usage:  python3 bench/connections.py build/kvserver [N=1000] [other/kvserver ...]

Starts each given server binary, opens N connections that each do one SET,
and reports the server's thread count, resident memory and virtual memory
(from /proc). Linux only.
"""
import os
import resource
import socket
import subprocess
import sys
import tempfile
import time

PORT = 19997


def status(pid):
    fields = {}
    with open(f"/proc/{pid}/status") as f:
        for line in f:
            key, _, value = line.partition(":")
            fields[key] = value.strip()
    return (int(fields["Threads"]), int(fields["VmRSS"].split()[0]) / 1024,
            int(fields["VmSize"].split()[0]) / 1024)


def measure(binary, n):
    workdir = tempfile.mkdtemp()
    proc = subprocess.Popen([binary, "-p", str(PORT)], cwd=workdir,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(0.5)
        idle = status(proc.pid)
        socks = [socket.create_connection(("127.0.0.1", PORT)) for _ in range(n)]
        for i, s in enumerate(socks):
            s.sendall(b"SET k%d v\n" % i)
        for s in socks:
            assert s.recv(16) == b"OK\n"
        time.sleep(0.3)
        busy = status(proc.pid)
        for s in socks:
            s.close()
        return idle, busy
    finally:
        proc.kill()
        proc.wait()


def main():
    binaries = [a for a in sys.argv[1:] if not a.isdigit()]
    counts = [int(a) for a in sys.argv[1:] if a.isdigit()]
    n = counts[0] if counts else 1000
    if not binaries:
        print(__doc__)
        return 2
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    resource.setrlimit(resource.RLIMIT_NOFILE, (min(hard, n + 1024), hard))
    print(f"| server | threads (idle → {n} conns) | RSS | virtual memory |")
    print("|---|---:|---:|---:|")
    for b in binaries:
        (t0, _, _), (t1, rss, vsz) = measure(b, n)
        print(f"| `{b}` | {t0} → {t1} | {rss:.1f} MB | {vsz:,.0f} MB |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
