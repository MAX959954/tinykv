#ifndef STORE_H
#define STORE_H
#include <stddef.h>
#include <stdint.h>

// The key-value store proper: in-memory hash map + write-ahead log.
// All functions are thread-safe. The network layer (server.c) only parses
// requests and formats replies; everything about concurrency and
// durability lives here, so it can be tested without sockets.

typedef enum {
    // Default. Reads take a shared lock and never wait for the disk; writes
    // are batched so that one fsync commits many of them.
    STORE_MODE_GROUP,
    // The original design, kept as a baseline for benchmarks: one global
    // lock held for every operation, including the fsync of every write.
    STORE_MODE_SERIAL,
    // Group mode without fsync. NOT durable -- benchmark upper bound only.
    STORE_MODE_NOSYNC,
} StoreMode;

typedef enum { STORE_OK, STORE_NOT_FOUND, STORE_ERROR } StoreResult;

typedef struct Store Store;

// Replays the log at wal_path (truncating a torn tail left by a crash),
// then opens it for appending. Returns NULL on failure.
Store *store_open(const char *wal_path, StoreMode mode);
void store_close(Store *s);

// OK once the write is durable (and visible to readers);
// ERROR if it could not be logged (memory is then unchanged).
StoreResult store_set(Store *s, const char *key, const char *value);

// OK if the key existed and the delete is durable; NOT_FOUND if absent.
StoreResult store_del(Store *s, const char *key);

// Copies the value into out (truncated to out_size - 1 bytes).
// Readers only ever see writes that are already durable.
StoreResult store_get(Store *s, const char *key, char *out, size_t out_size);

// Number of fsyncs performed so far (shows how well writes are batched).
uint64_t store_sync_count(Store *s);

#endif
