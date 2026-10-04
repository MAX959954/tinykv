#ifndef STORE_H
#define STORE_H
#include <stddef.h>
#include <stdint.h>

// The key-value store proper: in-memory hash map + write-ahead log +
// snapshots. All functions are thread-safe. The network layer (server.c)
// only parses requests and formats replies; everything about concurrency
// and durability lives here, so it can be tested without sockets.
//
// Writes are committed by one dedicated thread. It takes every write that
// is queued, appends them to the log with one write() and one fsync(),
// applies them to the map, and reports each result through a callback.
// Writes that arrive while it is busy form the next batch.

typedef enum {
    // Default. Reads take a shared lock and never wait for the disk; writes
    // are batched so that one fsync commits many of them.
    STORE_MODE_GROUP,
    // Baseline for benchmarks: one write per fsync, and the map stays
    // exclusively locked during the fsync, so reads wait for the disk too.
    STORE_MODE_SERIAL,
    // Group mode without fsync. NOT durable -- benchmark upper bound only.
    STORE_MODE_NOSYNC,
} StoreMode;

typedef enum {
    STORE_OK,
    STORE_NOT_FOUND,
    STORE_ERROR,
    STORE_PENDING,   // async call accepted; the callback will report the result
} StoreResult;

typedef struct {
    StoreMode mode;
    // Compact (snapshot + truncate the log) once the log grows past this
    // many bytes. 0 disables automatic compaction.
    uint64_t compact_bytes;
    // If the log is damaged in the middle (not just a torn tail), refuse to
    // start unless repair is set -- then truncate it at the damage, losing
    // every record from there on.
    int repair;
} StoreOptions;

typedef struct Store Store;

// Loads the snapshot (if any), replays the log on top of it, truncates a
// torn tail, and starts the commit thread. Returns NULL on failure (the
// reason is printed to stderr).
Store *store_open(const char *wal_path, const StoreOptions *opts);

// Commits every write still queued, stops the commit thread, frees all.
void store_close(Store *s);

// ---- asynchronous writes (used by the event loop) ----

// Called from the commit thread, exactly once per STORE_PENDING call, once
// the write is durable and visible to readers (OK / NOT_FOUND), or failed
// (ERROR). Must be quick and must not call back into the store.
typedef void (*store_done_fn)(void *ctx, StoreResult result);

// Return STORE_PENDING if the write was queued, otherwise the final result
// right away (e.g. NOT_FOUND for deleting a missing key, ERROR if the log
// has failed). key and value are copied; the caller's buffers are free to
// reuse immediately.
StoreResult store_set_async(Store *s, const char *key, const char *value,
                            store_done_fn done, void *ctx);
StoreResult store_del_async(Store *s, const char *key,
                            store_done_fn done, void *ctx);

// ---- synchronous API (submit + wait) ----

StoreResult store_set(Store *s, const char *key, const char *value);
StoreResult store_del(Store *s, const char *key);

// Copies the value into out (truncated to out_size - 1 bytes). Readers only
// ever see writes that are already durable. Never waits for the disk
// (except in STORE_MODE_SERIAL).
StoreResult store_get(Store *s, const char *key, char *out, size_t out_size);

// Snapshot the current state and truncate the log, now. Returns 0 on
// success. (Normally triggered automatically by compact_bytes.)
int store_compact(Store *s);

typedef struct {
    uint64_t keys;
    uint64_t wal_bytes;
    uint64_t syncs;          // fsyncs of the log so far
    uint64_t batches;        // commit batches so far
    uint64_t compactions;
} StoreStats;

StoreStats store_stats(Store *s);

#endif
