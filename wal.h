#ifndef WAL_H
#define WAL_H
#include <stddef.h>
#include <stdint.h>
#include "hashmap.h"
#include "command.h"

// Largest possible WAL record: "SET " + key + " " + value + "\n" + NUL.
// Both the writer and the reader (replay_log) size their buffers from
// this, so a record that was accepted can always be replayed.
#define WAL_MAX_RECORD (4 + MAX_KEY + 1 + MAX_LINE + 1 + 1)

// Append-only write-ahead log file. Not thread-safe by itself: the store
// guarantees only one thread (the current group-commit leader) writes at a
// time. Batching many records into one wal_write() call is what makes group
// commit pay off -- one fsync covers all of them.
typedef struct Wal Wal;

// Opens (creating if needed) the log for appending.
// sync = 0 skips fsync entirely (NOT durable; for benchmarking only).
// Returns NULL on failure (errno is set).
Wal *wal_open(const char *path, int sync);
void wal_close(Wal *w);

// Appends len bytes (one or more complete '\n'-terminated records) and,
// unless sync is off, fsyncs. Returns 0 once the data is durable, -1 on any
// I/O error. Failure is sticky: every later call returns -1 too, because a
// failed write may have left a partial record at the end of the file and
// anything appended after it would be unreadable on replay.
int wal_write(Wal *w, const char *data, size_t len);

// 1 if a write or fsync has failed (the log refuses further writes).
int wal_failed(const Wal *w);

// Number of fsync calls so far (shows how well writes are batched).
uint64_t wal_sync_count(const Wal *w);

// Rebuilds map from the log at path. A missing log is not an error.
// *valid_bytes receives the length of the log up to the end of the last
// complete record; anything after it is a torn write from a crash and must
// be truncated before appending, or the next record would be glued onto it.
// Returns 0 on success, -1 if the map ran out of memory.
int replay_log(const char *path, HashMap *map, long *valid_bytes);

#endif
