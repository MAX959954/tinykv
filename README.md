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

Line-based, one command per line, `\n`-terminated, max line length 512 bytes.

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

No automated test suite yet (see [Known limitations](#known-limitations)),
but the following was manually verified:

- **Persistence**: several `SET`s, `kill -9` the server mid-session,
  restart, `GET` the keys back — data survives.
- **Concurrency**: 8 clients issuing 1,600 concurrent `SET`/`GET` calls
  against the *same* key — no crashes, no corrupted values, and the WAL
  came out with every record intact and in order.
- **Malformed/edge-case input**: empty lines, `SET` with no value, a line
  split across multiple `recv()` calls, multiple commands coalesced into
  one `recv()`, and an oversized (>512 byte) line — all handled without
  crashing.
- **The `SIGPIPE` bug above** — found by testing a client that disconnects
  mid-pipeline, reproduced 5/5 times before the fix, 0/5 after.

## Known limitations

- No authentication, encryption, or rate limiting — trusted-network use only.
- No automated test suite — `parse_command` and the hash map are pure
  functions that are straightforward to unit-test with plain `assert()`,
  but that harness doesn't exist yet.
- Values are capped at just under 512 bytes (`MAX_LINE`), since a command
  line — verb, key, and value together — must fit in one line buffer.
- Thread-per-connection has no cap on concurrent connections; a large
  number of simultaneous clients means a large number of OS threads.
- IPv4 and IPv6 are both accepted (`AF_UNSPEC`), but this hasn't been
  tested over IPv6 specifically.