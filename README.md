# tinykv

[![CI](https://github.com/MAX959954/tinykv/actions/workflows/ci.yml/badge.svg)](https://github.com/MAX959954/tinykv/actions/workflows/ci.yml)

A persistent key-value store server in C11 for Linux, speaking a line-based
TCP protocol. Every acknowledged write is durable — `kill -9` the process
and the data is still there when it comes back up.

- **epoll event loop** — a few threads serve thousands of connections
- **group commit** — one `fsync` makes a whole batch of concurrent writes durable
- **checksummed write-ahead log** with **snapshots and log compaction**
- tested under ASan, UBSan and TSan, statically analyzed with clang-tidy,
  runs in Docker with one command

```
$ docker build -t tinykv . && docker run --rm -p 9999:9999 -v tinykv-data:/data tinykv
$ nc localhost 9999
SET user:1 alice
OK
GET user:1
alice
DEL user:1
OK
GET user:1
NOT_FOUND
```

## Contents

[Protocol](#protocol) · [Build and run](#build-and-run) ·
[Architecture](#architecture) · [On-disk format](#on-disk-format-and-recovery) ·
[Benchmarks](#benchmarks) · [Design decisions](#design-decisions) ·
[Testing](#testing) · [Known limitations](#known-limitations)

## Protocol

Line-based, one command per line, terminated by `\n` (`\r\n` from telnet and
similar clients is accepted too). Max line length 512 bytes including the
terminator, max key length 127 bytes. Commands may be pipelined; replies
always come back in request order.

| Command | Request | Reply |
|---|---|---|
| Set | `SET <key> <value>\n` | `OK\n`, or `ERROR\n` if the write could not be made durable |
| Get | `GET <key>\n` | `<value>\n` or `NOT_FOUND\n` |
| Delete | `DEL <key>\n` | `OK\n` or `NOT_FOUND\n` (`ERROR\n` as for `SET`) |
| Malformed | anything else | `ERROR\n` (connection stays open) |

`<value>` is everything after the key to the end of the line, so it may
itself contain spaces. A client beyond `--max-connections` receives
`ERROR too many connections` and is disconnected.

## Build and run

**Docker** (any OS — this is the easiest way on Windows or macOS):

```sh
docker build -t tinykv .            # compiles with -Werror and runs the whole test suite
docker run --rm -p 9999:9999 -v tinykv-data:/data tinykv
docker run --rm -p 9999:9999 tinykv --threads 4 --compact-bytes 1000000   # extra flags
```

**Natively** on Linux (gcc or clang, CMake ≥ 3.13; Python 3 for the
end-to-end tests):

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/kvserver --port 9999 --wal ./kv.log
```

| Flag | Meaning |
|---|---|
| `-p`, `--port PORT` | TCP port (default `9999`) |
| `-w`, `--wal PATH` | write-ahead log (default `kv.log`); the snapshot lives next to it as `PATH.snap` |
| `-t`, `--threads N` | event-loop threads (default: number of CPUs) |
| `-c`, `--max-connections N` | refuse clients beyond this (default 10000) |
| `-s`, `--compact-bytes N` | snapshot + truncate the log once it exceeds N bytes; `0` = never (default 64 MiB) |
| `-m`, `--mode MODE` | `group` (default): batched fsync · `serial`: one fsync per write, reads wait for it — the original design, kept as a benchmark baseline · `nosync`: no fsync at all, **not durable**, only to measure what fsync costs |
| `--repair` | if the log is damaged mid-file, truncate it there (losing what follows) instead of refusing to start |

`Ctrl+C` / `SIGTERM` / `docker stop` shut down gracefully: stop accepting,
finish the writes already in flight, send their replies, exit 0.

Sanitizer builds: `-DTINYKV_SANITIZE=address,undefined` or
`-DTINYKV_SANITIZE=thread`; `-DTINYKV_WERROR=ON` turns warnings into errors.

## Architecture

```
            ┌──────────── worker thread × N (epoll) ─────────────┐
 clients ──►│ own SO_REUSEPORT listener, own epoll, own eventfd  │
            │ parse lines → GET: answered inline (shared lock)   │
            │             → SET/DEL: queued ──────────┐          │
            │ eventfd wakes ◄── "write done" ──┐      │          │
            └──────────────────────────────────┼──────┼──────────┘
                                               │      ▼
                         ┌───────────── commit thread ─────────────┐
                         │ take whole queue → one write() + fsync()│──► kv.log
                         │ apply batch to map (exclusive lock)     │
                         │ notify each connection's worker         │
                         │ log too big → snapshot + truncate       │──► kv.log.snap
                         └─────────────────────────────────────────┘
```

**Connections** are served by `--threads` event-loop workers. Each worker
has its own listening socket on the same port (`SO_REUSEPORT` — the kernel
spreads new connections across them), its own `epoll` instance, and owns its
connections outright, so workers never contend with each other.

**Reads** are answered inline: take the map's lock in shared mode, copy the
value, done. They never touch the log and never wait for the disk.

**Writes** are handed to the store's single commit thread and the worker
moves on to other connections. The commit thread takes *everything* that is
queued, writes all of it with one `write()` and one `fsync()`, applies the
batch to the map under one exclusive lock, and reports each result: it puts
the connection on its worker's completion list and pokes the worker's
`eventfd`, and the worker sends the reply. Writes arriving during an
`fsync` form the next batch, so the slower the disk, the bigger the batches
— no timers, no tuning.

**One write per connection in flight.** After handing off a write, a
connection's remaining pipelined commands wait until it completes. That
keeps replies in request order and guarantees read-your-writes
(`SET a 1` / `GET a` in one packet always returns `1`) without any
per-connection sequencing machinery; batching happens across connections.

## On-disk format and recovery

**WAL records** — one per line, plain text but self-checking:

```
5a1e7c0b SET user:1 alice
0d3f9a21 DEL user:1
```

The first field is the CRC-32 of the rest of the line. On startup the log is
replayed until the first invalid record, and *where* it is decides what that
means:

| Situation | Meaning | What happens |
|---|---|---|
| invalid record is the last thing in the file (no `\n`, bad CRC, zero-filled block) | a crash in the middle of an append | normal: the tail is truncated and the server starts |
| invalid record with valid data after it | real damage in the middle of the log | **refuse to start**, report the offset; `--repair` truncates there, discarding the rest |

Without checksums those two cases are indistinguishable, and a flipped byte
would silently become a different value.

**Snapshots and compaction.** Once the log exceeds `--compact-bytes`, the
commit thread writes the whole map to `kv.log.snap` (same record format,
plus an `END <count>` trailer) and truncates the log. Startup loads the
snapshot, then replays the now-short log on top of it. The snapshot is
written to a temporary file, `fsync`ed, `rename`d into place, and the
directory is `fsync`ed — so at any crash point there is either the old
snapshot or the new one, never half of one. See
[Design decisions](#design-decisions) for why a crash between the rename and
the truncate is harmless.

## Benchmarks

`bench/kvbench` opens *C* connections (one client thread each); every client
sends requests one at a time and records each latency.
`bash bench/run_bench.sh build` runs every workload against a fresh server
in each mode; each number is the median of 3 runs.

Machine: 2-vCPU cloud VM, ext4 on a virtio disk; client and server share the
two CPUs, so absolute numbers will differ elsewhere — the ratios are the
point. Runs on this VM vary by up to ±20%, so differences smaller than that
are noise.

### Durability vs throughput (`--mode`)

**Writes (100% `SET`), ops/s**

| clients | `serial` (fsync per write) | `group` (default) | `nosync` (not durable) |
|---:|---:|---:|---:|
| 1 | 4,036 | 3,994 | 15,567 |
| 8 | 5,701 | 17,360 | 62,971 |
| 64 | 5,707 | 58,183 | 84,376 |
| 256 | 5,053 | **77,277** | 87,317 |

- `serial` is stuck at one `fsync` per write — ~5.5k writes/s no matter how
  many clients.
- Group commit scales with concurrency: **15× `serial` at 256 clients**, at
  ~88% of the no-fsync upper bound while staying fully durable.
- With one client there is nothing to batch: every write pays a full
  `fsync` (~160 µs of the ~225 µs p50 latency here; compare `nosync`).

**Mixed load (90% `GET` / 10% `SET`)**

| clients | ops/s `serial` | ops/s `group` | `GET` p99 `serial` | `GET` p99 `group` |
|---:|---:|---:|---:|---:|
| 8 | 29,992 | 68,601 | 847 µs | 253 µs |
| 64 | 31,421 | 103,775 | 4,760 µs | 1,017 µs |
| 256 | 33,051 | 86,841 | 17,367 µs | 6,585 µs |

In `serial` mode the map stays locked during every `fsync`, so reads queue
behind the disk; in `group` mode they never wait for it.

### Event loop vs thread-per-connection

The previous version of this server used one thread per connection, with
the same group-commit idea (writer threads took turns leading a batch). Same
benchmark, `group` mode:

| workload | clients | thread per connection | epoll event loop |
|---|---:|---:|---:|
| 100% `SET` | 1 | 5,113 | 3,994 (−22%) |
| 100% `SET` | 8 | 15,243 | 17,360 (+14%) |
| 100% `SET` | 64 | 39,344 | 58,183 (+48%) |
| 100% `SET` | 256 | 44,931 | **77,277 (+72%)**, p50 latency 5.4 → 1.9 ms |
| 100% `GET` | 64 | 98,240 | 118,343 (noise) |
| 90/10 mixed | 64 | 117,146 | 103,775 (noise) |

And what connections cost (`python3 bench/connections.py OLD NEW 1000`):

| | threads, idle → 1000 connections | RSS | virtual memory |
|---|---:|---:|---:|
| thread per connection | 1 → **1001** | 14.0 MB | 8,583 MB |
| epoll event loop (2 workers) | 4 → **4** | 3.8 MB | 219 MB |

The honest summary: the event loop is not a free speed-up. A single client's
write is ~30–50 µs slower (p50 181 → 229 µs), because it now crosses threads twice (worker →
commit thread → worker) instead of the client's own thread doing the
`fsync`. In exchange, write throughput keeps scaling where the old design
flattened out, and a connection costs a few kilobytes instead of a thread
with an 8 MB stack reservation. Read-heavy loads are about the same.

### Compaction and startup

Measured with the store directly (`group` mode, 38-byte values):

| keys | replay a log of N `SET`s | compaction (write + fsync snapshot) | startup from snapshot |
|---:|---:|---:|---:|
| 10,000 | 4 ms | 6 ms | 4 ms |
| 100,000 | 56 ms | 81 ms | 38 ms |
| 1,000,000 | 664 ms | 873 ms | 417 ms |

Writes are paused while a snapshot is written (they queue up and form one
big batch afterwards); reads continue.

## Design decisions

**epoll workers, with writes off the event loop.** A worker must never block
on the disk, or one `fsync` would stall every connection it serves. So
writes go to a dedicated commit thread and complete asynchronously through
an `eventfd`. The commit thread is the only writer of both the log and the
map, which makes ordering trivial: batches are applied in exactly the order
they were written to the log.

**Completion without allocation or locking the connection.** The commit
thread only links the connection into its worker's completion list (one
mutex) and writes to the `eventfd`; it never touches the socket or the
`epoll` set. Each connection has at most one write in flight, so the
connection struct itself is the list node.

**Freeing connections safely.** A connection can be closed while its write
is still with the commit thread (the client disconnected) — it is then only
freed when the completion arrives. And one `epoll_wait` batch can contain
an event for a connection that an earlier event in the same batch closed, so
closed connections go on a per-worker list that is freed at the end of the
loop iteration, not immediately. (Both found in review before the first run;
the end-to-end tests run under AddressSanitizer to keep it that way.)

**Group commit adapts by itself.** While the commit thread is in `fsync`,
new writes queue up; the next batch is everything that arrived. Under light
load batches are small and latency is one `fsync`; under heavy load batches
grow (up to 256 records) and throughput approaches the no-fsync bound.

**Reader-writer lock, writer-preferring.** `GET`s share the map lock; the
commit thread holds it exclusively only to apply a batch, never during
`fsync`. glibc's default `pthread_rwlock` prefers readers, so a steady
stream of `GET`s could starve the commit thread forever; the lock is created
with `PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP`.

**Log, then apply, then acknowledge.** A write is applied to memory and
acknowledged only after its record is `fsync`ed. If the reply came first, a
crash could lose a write the client was told succeeded. Applying after
logging matters too: otherwise a `GET` could return a value that a crash
would then undo.

**Compaction is crash-safe because replay is idempotent.** The log is
truncated only after the new snapshot has been atomically renamed into
place. A crash between the two leaves the new snapshot *and* the full old
log, and startup replays that log on top of a snapshot that already
contains its effects. That is harmless: after replaying a sequence of
`SET`/`DEL` records, each key ends up with the result of the last record
that touched it, whether the sequence is applied once or twice. (Tested by
restoring the pre-compaction log after a compaction and restarting.)

**A failed log write makes the server read-only.** If `write` or `fsync`
fails (disk full, I/O error), every write in that batch gets `ERROR`, memory
is left untouched, and later writes are refused until a restart: after a
failed write the log may end in a partial record, and after a failed
`fsync` the kernel may have dropped the dirty pages, so retrying and
reporting success could be a lie. Reads keep working.

**Out of memory after a write is logged → abort.** If the record is already
durable but the map can't allocate memory to apply it, replying `ERROR`
would be wrong (the write reappears after a restart). The server aborts; the
log is the source of truth and the next start rebuilds a consistent state.
All other allocation failures are handled — a failed allocation for one
connection drops only that connection.

**Growable hash map.** The map doubles its bucket array when the load factor
would exceed 3/4. Nodes cache their 64-bit FNV-1a hash, so a resize relinks
nodes without rehashing strings, and the bucket count is a power of two so
the index is a mask. If the bigger array can't be allocated the map simply
stays at its current size — still correct, just slower.

**Graceful shutdown with `sigwait`, not a signal handler.** `SIGINT` and
`SIGTERM` are blocked in every thread; `main` waits for them with
`sigwait()`, then sets a flag and wakes each worker through its `eventfd`.
No code runs in signal-handler context. This also matters in Docker, where
the server is PID 1: a PID-1 process gets no default signal actions, so a
server that doesn't handle `SIGTERM` itself is only stopped by the
`SIGKILL` that `docker stop` sends after 10 seconds.

**Ignoring `SIGPIPE`.** Writing to a socket whose peer has closed raises
`SIGPIPE`, which by default kills the process — *every* client's connection
over one misbehaving client. Concretely: a client that pipelines several
commands and disconnects after reading only the first reply makes the next
`send()` kill the whole server. Sends use `MSG_NOSIGNAL` and `SIGPIPE` is
ignored, so a write to a closed socket fails with `EPIPE` and only that
connection is dropped. This was found by testing, not by inspection.

**Text protocol, fixed 512-byte line buffer.** Easy to debug with `nc`, and
the per-connection memory is bounded: 512 bytes of input plus at most
~64 KB of pending replies (a client that pipelines but doesn't read its
replies stops being read from until it does).

## Project layout

| Path | Responsibility |
|---|---|
| `server.c` | Flags, epoll workers, connection state machine, graceful shutdown |
| `store.c/h` | The store: commit thread, group commit, async/sync API, recovery, compaction |
| `wal.c/h` | Checksummed record format, the log file, replay with torn-tail/corruption detection |
| `snapshot.c/h` | Atomic snapshot write and verified load |
| `hashmap.c/h` | Growable chained hash map |
| `crc32.c/h` | CRC-32 (IEEE) |
| `linebuf.c/h` | Reassembles a TCP byte stream into lines |
| `command.c/h` | Parses a line into a command |
| `tests/test_*.c` | Unit tests, one per module (plain C, no framework) |
| `tests/regression_test.py` | End-to-end tests against a real server process |
| `bench/` | `kvbench` load generator, `run_bench.sh`, `connections.py` |
| `Dockerfile` | Build + test stage, minimal non-root runtime stage |
| `.clang-tidy` | Static-analysis configuration (warnings are errors) |
| `.github/workflows/ci.yml` | CI, see below |

## Testing

`ctest` runs six unit-test binaries and the end-to-end suite. CI runs
them with gcc and clang, each plain, under AddressSanitizer +
UndefinedBehaviorSanitizer, and under ThreadSanitizer, with `-Werror`; runs
clang-tidy over every source file; builds the Docker image (which runs the
tests again), smoke-tests it and checks that `docker stop` exits cleanly.
The end-to-end suite fails a test if the server's stderr contains any
sanitizer report, including leaks found at graceful shutdown.

**Unit tests** (`tests/test_*.c`):
- record format: CRC-32 check values, every single flipped character of a
  record is detected
- replay: torn tail (partial record, bad last record, zero-filled block) vs
  corruption in the middle; valid-prefix length and offsets
- snapshots: round trip of 1,000 keys; truncated, missing trailer, wrong
  count, flipped byte and trailing garbage are all rejected
- store: persistence, torn-tail truncation, refusing a corrupt log unless
  `repair`, manual and automatic compaction, **a crash between snapshot
  rename and log truncation**, leftover temp files, async API and draining
  the queue on close, failing disk (`/dev/full`), group-commit batching, and
  **concurrent random `SET`/`DEL`/`GET` from 8 threads on 8 keys — in every
  mode and while compacting — after which memory must equal a fresh replay
  of snapshot + log**
- hash map: growth keeps the load factor ≤ 3/4, deletes across resizes,
  iteration visits each entry once; parser and line buffer edge cases

**End-to-end tests** (`tests/regression_test.py`) — each starts a real
server on a free port in a fresh directory:
- **crash under load**: 8 clients write concurrently, `SIGKILL` mid-stream,
  and after restart every acknowledged write must be present (~16k per run)
- **graceful shutdown** under load with `SIGTERM` and `SIGINT`: exit code 0,
  every acknowledged write present
- pipelined `SET`/`GET`/`DEL` in one packet come back in order; a client
  that half-closes its socket still gets every reply
- 300 concurrent connections served by 4 threads; `--max-connections`
  enforced and freed slots reusable
- compaction keeps the log under the threshold; recovery from snapshot + log
  after a crash
- a corrupt log refuses to start and names `--repair`; `--repair` keeps the
  valid prefix
- over-long keys and lines, CRLF clients, a failing disk

**Bugs found along the way** (each has a test now):
- **A maximum-length `SET` broke WAL replay**: its record was truncated,
  losing its `\n`, and replay stopped there — silently dropping every later
  write.
- **Long keys were silently truncated** by `sscanf("%127s")`, the remainder
  leaking into the value.
- **A failed disk write was still acknowledged with `OK`** — return values
  of `fputs`/`fflush`/`fsync` were ignored and memory was updated before the
  log.
- **A torn record corrupted the next write** after a restart, which was
  appended directly after the fragment.
- **CRLF clients stored a stray `\r`**; **valid pipelined input could
  disconnect a client** when a partial line plus the next chunk exceeded the
  line buffer.
- **The first group-commit version was slower than no group commit** at 64
  clients: every commit woke all waiting threads through one shared
  condition variable (a thundering herd). Caught by the benchmark.
- clang-tidy: the benchmark client leaked its buffers and parsed numbers
  with `atoi` (garbage silently became 0).

## Known limitations

- Linux only (epoll, eventfd, `SO_REUSEPORT`); use Docker elsewhere.
- No authentication, encryption, or rate limiting — trusted networks only.
- Compaction pauses writes for the duration of the snapshot (~80 ms per
  100k keys here). A fork-based or incremental snapshot would avoid that.
- A single client's write latency is ~30–50 µs higher than in the
  thread-per-connection version (two thread hand-offs per write).
- Resizing the hash map rehashes all entries at once under the write lock;
  with millions of keys that is a noticeable pause (incremental rehashing,
  as Redis does, would spread it out).
- Keys up to 127 bytes, values just under 512 bytes (a whole command must
  fit in one line buffer).
- Idle connections are never timed out.
- IPv4 and IPv6 are both accepted (`AF_UNSPEC`), but IPv6 isn't tested.
