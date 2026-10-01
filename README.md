# tinykv

[![CI](https://github.com/MAX959954/tinykv/actions/workflows/ci.yml/badge.svg)](https://github.com/MAX959954/tinykv/actions/workflows/ci.yml)

A small persistent key-value store server written in C, speaking a line-based
TCP protocol. Multiple clients can connect concurrently; every write is
durable — kill the process mid-session and the data is still there when it
comes back up. Writes are **group-committed** (one `fsync` covers many
concurrent writes) and reads never wait for the disk.

```
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

## Features

- **TCP server** with a minimal text protocol — `SET`, `GET`, `DEL`
- **Correct stream reassembly** — a per-connection line buffer that handles
  partial reads and multiple pipelined commands arriving in a single `recv()`
- **Durability** — a write-ahead log (WAL), `fsync`'d before any `SET`/`DEL`
  is acknowledged, replayed on startup so a `kill -9` mid-session doesn't
  lose data; a torn record left by a crash is detected and truncated
- **Group commit** — concurrent writes are batched into one `write()` +
  one `fsync()`: **~7× the write throughput** of fsync-per-write at 64
  clients ([Benchmarks](#benchmarks))
- **Concurrency** — thread-per-connection; the hash map is guarded by a
  reader-writer lock, so `GET`s run in parallel and never wait on `fsync`
- **Tested** — unit tests per module, end-to-end tests including crash
  recovery under concurrent load, run in CI under ASan, UBSan and TSan
- **Hardened** against disconnecting clients, partial `send()`s, full disks
  and allocation failures (see [Design decisions](#design-decisions))

## Protocol

Line-based, one command per line, terminated by `\n` (a `\r\n` from telnet
and similar clients is accepted too). Max line length 512 bytes including the
terminator, max key length 127 bytes.

| Command | Request | Reply |
|---|---|---|
| Set | `SET <key> <value>\n` | `OK\n`, or `ERROR\n` if the write could not be made durable |
| Get | `GET <key>\n` | `<value>\n` or `NOT_FOUND\n` |
| Delete | `DEL <key>\n` | `OK\n` or `NOT_FOUND\n` (`ERROR\n` as for `SET`) |
| Malformed | anything else | `ERROR\n` (connection stays open) |

`<value>` is everything after the key to the end of the line, so it may
itself contain spaces.

## Build, test, run

Linux or macOS (POSIX sockets and threads; on Windows use WSL).

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure

./build/kvserver                 # port 9999, log ./kv.log
./build/kvserver -p 7000 -w /var/lib/tinykv/kv.log
```

| Flag | Meaning |
|---|---|
| `-p port` | TCP port (default `9999`) |
| `-w path` | write-ahead log file (default `kv.log`); created, or replayed if it exists |
| `-m group` | rwlock + group commit (default) |
| `-m serial` | the original design — one global lock, `fsync` per write. Kept as a benchmark baseline |
| `-m nosync` | group mode without `fsync`. **Not durable**; only to measure what `fsync` costs |

Sanitizer builds: `cmake -B build -DTINYKV_SANITIZE=address,undefined` or
`-DTINYKV_SANITIZE=thread`. `-DTINYKV_WERROR=ON` turns warnings into errors
(CI uses both).

## Architecture

```
 client ──TCP──► server.c ──────► store.c ───────────────► wal.c ──► kv.log
                 thread per       │ rwlock-guarded          append +
                 connection,      │ hashmap.c               fsync
                 linebuf.c +      │ group-commit queue
                 command.c        └─ GET: shared lock only, never touches the WAL
```

**Write path (group commit).** Each `SET`/`DEL` formats its WAL record and
joins a queue, then sleeps on its own condition variable. The writer at the
head of the queue becomes the *leader*:

1. takes the whole queue (up to 128 writes) as one batch,
2. writes all their records with a single `write()` and one `fsync()` —
   without holding the queue lock, so new writers keep queueing behind it,
3. takes the map's write lock once and applies the batch in queue order,
4. wakes every writer in the batch individually and hands leadership to the
   next queued writer, whose batch is everything that arrived meanwhile.

The slower the disk, the bigger the batches get — the system adapts to load
by itself, with no timers or tuning knobs.

**Read path.** `GET` takes the map's lock in shared mode and copies the value
out. It never waits for a disk flush, and it only ever sees writes that are
already durable (records are applied to memory only after their `fsync`).

## Benchmarks

`bench/kvbench` opens *C* connections (one thread each); every client sends
requests one at a time and records each request's latency.
`bash bench/run_bench.sh build` runs every workload against a fresh server in
each mode and prints the tables below.

Machine: 2-vCPU cloud VM, ext4 on a virtio disk (`fsync` ≈ 185 µs); client
and server on the same machine, so they compete for the two CPUs. Absolute
numbers will differ on other hardware — the ratios are the point.

**Writes — 100% `SET`, ops/s**

| clients | `serial` (before) | `group` (after) | `nosync` (no fsync, not durable) |
|---:|---:|---:|---:|
| 1 | 6,364 | 5,809 | 31,955 |
| 8 | 6,830 | 19,461 | 88,213 |
| 64 | 6,680 | **45,101** | 84,813 |

At 64 clients, `SET` latency drops from p50 9.2 ms / p99 20.6 ms (`serial`)
to p50 1.4 ms / p99 2.2 ms (`group`).

**Mixed — 90% `GET` / 10% `SET`**

| clients | ops/s `serial` | ops/s `group` | `GET` p99 `serial` | `GET` p99 `group` |
|---:|---:|---:|---:|---:|
| 8 | 38,172 | 69,884 | 738 µs | 220 µs |
| 64 | 38,558 | **105,249** | 6,158 µs | **954 µs** |

What the numbers say:

- **`fsync` is the cost of durability.** With one client, a durable `SET`
  takes ~155 µs, of which ~125 µs is `fsync` (`nosync`: ~30 µs).
- **`serial` can't scale past one `fsync` at a time** — ~6.5k writes/s no
  matter how many clients. Group commit amortizes one `fsync` over many
  writes: 7× the throughput at 64 clients, and 53% of the no-fsync upper
  bound while staying fully durable.
- **Reads stop paying for writes.** Under `serial`, a `GET` queues behind
  every in-flight `fsync`; with the rwlock it doesn't, so `GET` p99 under a
  mixed load drops 6×.
- **One client gains nothing** — there is nothing to batch, so `group` and
  `serial` are within noise of each other.
- **Read-only load is unchanged** (~33k ops/s at 1 client, 100–130k at 8–64,
  for all modes; full table in the script output): with no writers there is
  nothing to wait for, and at this scale the 2 CPUs and the TCP round trips
  are the limit, not the lock.

The unit tests show the batching directly: 1,600 durable `SET`s from 8
threads take ~370 `fsync`s in `group` mode vs. ~1,600 in `serial`.

## Design decisions

**Thread-per-connection over an event loop.** Each accepted connection gets
its own thread (`pthread_create`), rather than multiplexing every socket on
one thread with `select()`/`poll()`. This trades lower memory/scheduling
efficiency at very high connection counts for simpler per-connection code —
each thread just blocks on `recv()` like a single-client program would.
Shared state lives behind `store.h`, which is the only module that deals
with locking and durability; the network code never touches a lock.

**Reader-writer lock, writer-preferring.** `GET`s share the map lock;
writers hold it exclusively, but only for the in-memory update of a batch,
never during `fsync`. glibc's default `pthread_rwlock` prefers readers, so a
steady stream of `GET`s could starve writers forever; the lock is created
with `PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP` on glibc.

**Group commit, LevelDB-style.** See [Architecture](#architecture). Two
details matter for correctness:
- *Apply in log order.* Batches are applied in queue order, which is exactly
  the order of the records in the file — so after two concurrent `SET`s of
  the same key, memory holds the same value a replay of the log would. The
  unit tests check this: 8 threads hammer 8 keys with random `SET`/`DEL`/`GET`,
  then the in-memory state is compared to a fresh replay of the log.
- *One condition variable per writer.* A first version used one shared
  condition variable with `pthread_cond_broadcast` — and the benchmark
  caught it: at 64 clients every commit woke all 64 threads to let one
  proceed, and throughput *fell below* the serial baseline (4.7k ops/s, even
  with `fsync` off). Waking each writer individually fixed it (45k ops/s).

**WAL ordering: log, then apply, then acknowledge.** A write is applied to
memory and acknowledged only after its record is `fsync`'d. If the fsync
happened after the reply, a crash between the two could leave the client
believing a write succeeded when it isn't on disk. Applying to memory after
logging matters too: otherwise a `GET` could briefly return a value that a
crash would then undo.

**Torn tail truncation.** A crash in the middle of an append leaves a
partial record at the end of the log. Replay stops there — and then
truncates the file to the last complete record. Without that, the first
record written after the restart would be glued onto the fragment
(`SET k vaSET x 1\n`), turning a harmless torn write into a corrupt record.

**A failed WAL write makes the server read-only.** If `write` or `fsync`
fails (disk full, I/O error), every write in that batch gets `ERROR` and
memory is left untouched. Every later write is refused too, until a restart:
after a failed write the log may end in a partial record, and after a failed
`fsync` the kernel may have dropped the dirty pages, so retrying and
reporting success could be a lie. Reads keep working in the meantime.

**Out of memory after a write is logged → abort.** If the record is already
durable but the hash map can't allocate memory to apply it, replying `ERROR`
would be wrong (the write reappears after a restart). The server aborts
instead; the WAL is the source of truth, and replay rebuilds a consistent
state on the next start. Allocation failures everywhere else (startup,
replay, thread creation) are checked and handled — a failed
`pthread_create` drops only that one client.

**Text protocol, not binary.** Easier to debug by hand with `nc`, at the
cost of a small amount of parsing/formatting overhead per command — not a
meaningful tradeoff at this scale.

**Partial `send()`/`write()`.** A socket or file may accept only part of a
buffer, so both go through loops that retry until every byte is written
(and on `EINTR`).

**Ignoring `SIGPIPE`.** By default, writing to a socket after the peer has
closed their end raises `SIGPIPE`, and a process that doesn't handle it is
killed — for *any* write, on *any* connection. Concretely: a client that
pipelines several commands (e.g. `SET a 1\nSET b 2\nGET a\nGET b\n`) and
disconnects after reading only the first reply leaves the server mid-way
through sending the rest — the next `send()` on that socket raises
`SIGPIPE` and, without this fix, terminates the *entire server*, dropping
every other connected client along with it. `main()` calls
`signal(SIGPIPE, SIG_IGN)`, so a write to a closed socket instead fails with
`EPIPE`, and the connection thread simply drops that one client. This was
found by testing, not by inspection — a single misbehaving client silently
killing the whole process is exactly the kind of bug that's invisible until
you go looking for it.

**Fixed-size line buffer (512 bytes) over a growable one.** A `LineBuf` per
connection just refuses (and disconnects) a line that doesn't fit rather
than reallocating. Simpler and bounds the memory any single connection can
consume, at the cost of rejecting legitimately long values.

## Project layout

| Path | Responsibility |
|---|---|
| `server.c` | Command-line flags, listener, accept loop, per-connection thread, reply formatting |
| `store.c/h` | The store: rwlock-guarded map, group-commit queue, ordered apply, startup replay |
| `wal.c/h` | Append-only log file (`write` + `fsync`, sticky failure) and replay |
| `hashmap.c/h` | In-memory chained hash map |
| `linebuf.c/h` | Reassembles a TCP byte stream into discrete lines |
| `command.c/h` | Parses a line into a `Command` (verb + key + value) |
| `tests/test_*.c` | Unit tests, one per module (plain C, no framework) |
| `tests/regression_test.py` | End-to-end tests over TCP, including crashes |
| `bench/` | `kvbench` load generator and `run_bench.sh` |
| `.github/workflows/ci.yml` | gcc + clang × {none, ASan+UBSan, TSan}, plus a benchmark smoke run |

## Testing

`ctest` runs everything: five unit-test binaries and the end-to-end suite.
CI runs it for gcc and clang, each plain, under AddressSanitizer +
UndefinedBehaviorSanitizer, and under ThreadSanitizer, with warnings as
errors. The end-to-end suite fails a test if the server's stderr contains
any sanitizer report.

**Unit tests** (`tests/test_*.c`) — the parser (limits, malformed input),
the line buffer (partial lines, pipelining, CRLF, capacity), the hash map
(collisions, deletes from any chain position, 10k keys), the WAL (replay,
torn tail, sticky failure on `/dev/full`), and the store: persistence across
reopen, torn-tail truncation, group-commit batching, a failing disk, and the
concurrency test that compares memory against a fresh replay of the log.

**End-to-end tests** (`tests/regression_test.py`) — each starts a real
server on a free port in a fresh directory:
- **crash under load**: 8 clients write concurrently, the server is
  `SIGKILL`ed mid-stream, and after restart every acknowledged write must be
  present (~14k writes per run)
- a maximum-length record and a `DEL` surviving a crash + replay
- over-long keys rejected, CRLF clients, malformed input
- pipelined commands split across `recv()` boundaries; oversized lines
  dropping only the offending client
- a failing disk (`kv.log` → `/dev/full`): writes get `ERROR`, nothing is
  applied in memory, reads still work

Bugs found this way (each now has a test):

- **A maximum-length `SET` broke WAL replay.** The record buffer was the same
  size as the input line, so `"SET <key> <value>\n"` could be truncated —
  losing its `\n`. On restart, replay treated it as a torn write and stopped,
  silently dropping every write acknowledged after it.
- **Long keys were silently truncated.** `sscanf("%127s")` cut the key at 127
  bytes and the remainder leaked into the value.
- **A failed disk write was still acknowledged with `OK`.** The return values
  of `fputs`/`fflush`/`fsync` were ignored, and the map was updated before
  the log, so a write that never reached disk was both visible to `GET` and
  confirmed to the client.
- **A torn record could corrupt the next write.** After a crash mid-append,
  the next record was appended directly after the fragment.
- **CRLF clients stored a stray `\r`.** `SET k v` from telnet stored `"v\r"`.
- **Valid pipelined input could disconnect a client.** A whole `recv()` chunk
  was appended to the line buffer at once, so a partial line plus a chunk
  carrying its end and more commands could exceed 512 bytes even though every
  line was valid.
- **The first group-commit version was slower than no group commit** at 64
  clients (thundering herd on a shared condition variable) — caught by the
  benchmark, see [Design decisions](#design-decisions).

## Known limitations

- No authentication, encryption, or rate limiting — trusted-network use only.
- The log grows forever: there is no compaction/snapshotting, so startup
  replay time grows with the total number of writes ever made.
- No per-record checksums: a torn tail is detected (missing `\n`), but a
  corrupted byte in the middle of the log is not.
- Keys are capped at 127 bytes (`MAX_KEY`); values at just under 512 bytes
  (`MAX_LINE`), since a command line — verb, key, and value together — must
  fit in one line buffer.
- The hash map has a fixed number of buckets (1024) and doesn't resize.
- Thread-per-connection has no cap on concurrent connections; a large
  number of simultaneous clients means a large number of OS threads.
- IPv4 and IPv6 are both accepted (`AF_UNSPEC`), but this hasn't been
  tested over IPv6 specifically.
