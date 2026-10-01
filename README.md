# tinykv

A small persistent key-value store server written in C, speaking a line-based
TCP protocol. Multiple clients can connect concurrently; every write is
durable — kill the process mid-session and the data is still there when it
comes back up.

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
  lose data
- **Concurrency** — thread-per-connection, with a mutex guarding the hash
  map and the WAL so concurrent writers can't corrupt either
- **Hardened against a disconnecting client** — a client that drops the
  connection mid-response can't take down the whole server (see
  [Design decisions](#design-decisions))

## Protocol

Line-based, one command per line, `\n`-terminated, max line length 512 bytes (including the `\n`), max key length
127 bytes.

| Command | Request | Reply |
|---|---|---|
| Set | `SET <key> <value>\n` | `OK\n` |
| Get | `GET <key>\n` | `<value>\n` or `NOT_FOUND\n` |
| Delete | `DEL <key>\n` | `OK\n` or `NOT_FOUND\n` |
| Malformed | anything else | `ERROR\n` (connection stays open) |

`<value>` is everything after the second space to the end of the line, so it
may itself contain spaces.

## Build

With `gcc` directly:

```sh
gcc -Wall -Wextra -std=c11 -pthread -o kvserver server.c linebuf.c command.c hashmap.c wal.c
```

Or with CMake:

```sh
cmake -B build
cmake --build build
```

## Run

```sh
./kvserver
```

Listens on port `9999` on all interfaces. A write-ahead log, `kv.log`, is
created (or replayed, if it already exists) in the working directory.

## Design decisions

**Thread-per-connection over an event loop.** Each accepted connection gets
its own thread (`pthread_create`), rather than multiplexing every socket on
one thread with `select()`/`poll()`. This trades lower memory/scheduling
efficiency at very high connection counts for simpler per-connection code —
each thread just blocks on `recv()` like a single-client program would. The
cost is that shared state (the hash map, the WAL file) is now touched from
multiple threads at once, so both are guarded by a single
`pthread_mutex_t` — every `SET`/`GET`/`DEL` takes the lock for its full
duration, including the WAL write. That serializes all writes, which is the
simplest way to guarantee the WAL's on-disk order always matches the order
operations actually happened in.

**Text protocol, not binary.** Easier to debug by hand with `nc`, at the
cost of a small amount of parsing/formatting overhead per command — not a
meaningful tradeoff at this scale.

**WAL ordering: log, then acknowledge.** `wal_write()` does
`fputs` → `fflush` → `fsync` *before* `dispatch()` replies `OK` to the
client. If the fsync happened after the reply, a crash between the two could
leave the client believing a write succeeded when it isn't actually on disk
— "durable" would be a lie. The tradeoff is latency: every write pays for a
synchronous disk flush before the client sees a response.

**Ignoring `SIGPIPE`.** By default, writing to a socket after the peer has
closed their end raises `SIGPIPE`, and a process that doesn't handle it is
killed — for *any* write, on *any* connection. Concretely: a client that
pipelines several commands (e.g. `SET a 1\nSET b 2\nGET a\nGET b\n`) and
disconnects after reading only the first reply leaves the server mid-way
through sending the rest — the next `send()` on that socket raises
`SIGPIPE` and, without this fix, terminates the *entire server*, dropping
every other connected client along with it. `main()` now calls
`signal(SIGPIPE, SIG_IGN)`, so a write to a closed socket instead fails with
`EPIPE`, and `handle_client` checks `send()`'s return value and simply drops
that one connection. This was found and confirmed by testing (see below),
not by inspection — a single misbehaving client silently killing the whole
process is exactly the kind of bug that's invisible until you go looking
for it.

**Fixed-size line buffer (512 bytes) over a growable one.** A `LineBuf` per
connection just refuses (and disconnects) a line that doesn't fit rather
than reallocating. Simpler and bounds the memory any single connection can
consume, at the cost of rejecting legitimately long values.

## Project layout

| File | Responsibility |
|---|---|
| `server.c` | Listener setup, accept loop, per-connection thread, command dispatch |
| `linebuf.c/h` | Reassembles a TCP byte stream into discrete `\n`-delimited lines |
| `command.c/h` | Parses a line into a `Command` (verb + key + value) |
| `hashmap.c/h` | In-memory chained hash map (the actual key-value storage) |
| `wal.c/h` | Write-ahead log: append durable records, replay them on startup |

## Testing

Regression tests live in `tests/regression_test.py`. Each test starts the
server in a fresh temporary directory, talks to it over TCP, and — where
durability is involved — `SIGKILL`s and restarts it:

```sh
cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure
# or directly:
python3 tests/regression_test.py build/kvserver
```

Covered: basic `SET`/`GET`/`DEL`/malformed input, a maximum-length record
surviving a crash + replay, over-long keys being rejected, pipelined
commands split across `recv()` boundaries, and oversized lines dropping only
the offending client.

Bugs found and fixed this way (each now has a regression test):

- **A maximum-length `SET` broke WAL replay.** The record buffer was the same
  size as the input line, so `"SET <key> <value>\n"` could be truncated — losing
  its `\n`. On restart, `replay_log` treated it as a torn write and stopped,
  silently dropping every write acknowledged after it. Record buffers are now
  sized from `WAL_MAX_RECORD`, and truncation is checked before anything is
  applied.
- **Long keys were silently truncated.** `sscanf("%127s")` cut the key at 127
  bytes and the remainder leaked into the value. Keys over `MAX_KEY` are now
  rejected with `ERROR`.
- **Valid pipelined input could disconnect a client.** A whole `recv()` chunk
  was appended to the line buffer at once, so a partial line plus a chunk
  carrying its end and more commands could exceed 512 bytes even though every
  line was valid. Input is now fed in pieces, draining complete lines between
  them.

Also verified manually: 8 clients issuing 1,600 concurrent `SET`/`GET` calls
against the same key (no crashes, WAL intact and in order), and the `SIGPIPE`
bug described above (reproduced 5/5 before the fix, 0/5 after).

## Known limitations

- No authentication, encryption, or rate limiting — trusted-network use only.
- No unit tests yet for the pure modules (`parse_command`, `linebuf`, the
  hash map) — only end-to-end regression tests over TCP.
- Keys are capped at 127 bytes (`MAX_KEY`); values at just under 512 bytes
  (`MAX_LINE`), since a command line — verb, key, and value together — must
  fit in one line buffer.
- Thread-per-connection has no cap on concurrent connections; a large
  number of simultaneous clients means a large number of OS threads.
- IPv4 and IPv6 are both accepted (`AF_UNSPEC`), but this hasn't been
  tested over IPv6 specifically.