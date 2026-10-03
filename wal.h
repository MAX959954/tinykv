#ifndef WAL_H
#define WAL_H
#include <stddef.h>
#include <stdint.h>
#include "hashmap.h"
#include "command.h"

// ---- record format ----
//
// One record per line:   <crc32 as 8 hex digits> <payload>\n
// where payload is "SET <key> <value>" or "DEL <key>" and the CRC covers
// exactly the payload bytes. Example:
//
//     5a1e7c0b SET user:1 alice
//
// Still plain text (you can read the log with less), but every record is
// self-checking: a flipped byte anywhere is detected on replay.

#define WAL_CRC_PREFIX 9   // 8 hex digits + 1 space

// Largest possible record: crc + "SET " + key + " " + value + "\n" + NUL.
#define WAL_MAX_RECORD (WAL_CRC_PREFIX + 4 + MAX_KEY + 1 + MAX_LINE + 1 + 1)

// Formats payload into a record (crc prefix + payload + '\n') in buf.
// Returns the record length, or 0 if it doesn't fit in cap bytes.
size_t wal_format_record(char *buf, size_t cap, const char *payload);

// Checks a record's CRC (line without its trailing '\n') and copies the
// payload, NUL-terminated, into out. Returns 0 if valid, -1 otherwise.
int wal_record_payload(const char *line, size_t len, char *out, size_t cap);

// Checks a record (without its trailing '\n') and parses its payload.
// Returns 0 if the CRC matches and the payload parses, -1 otherwise.
int wal_parse_record(const char *line, size_t len, Command *cmd);

// ---- the log file ----
//
// Append-only. Not thread-safe by itself: only the store's commit thread
// writes to it. Batching many records into one wal_write() call is what
// makes group commit pay off -- one fsync covers all of them.
typedef struct Wal Wal;

// Opens (creating if needed) the log for appending.
// sync = 0 skips fsync entirely (NOT durable; for benchmarking only).
// Returns NULL on failure (errno is set).
Wal *wal_open(const char *path, int sync);
void wal_close(Wal *w);

// Appends len bytes (one or more complete records) and, unless sync is off,
// fsyncs. Returns 0 once the data is durable, -1 on any I/O error. Failure
// is sticky: every later call returns -1 too, because a failed write may
// have left a partial record at the end of the file and anything appended
// after it would be unreadable on replay.
int wal_write(Wal *w, const char *data, size_t len);

// Empties the log (after its contents were captured in a snapshot).
// Returns 0 on success; on failure the log is marked failed.
int wal_truncate(Wal *w);

int wal_failed(const Wal *w);
uint64_t wal_size(const Wal *w);         // current length in bytes
uint64_t wal_sync_count(const Wal *w);   // fsyncs so far (shows batching)

// ---- replay ----

typedef enum {
    REPLAY_OK,          // every record valid
    REPLAY_TORN_TAIL,   // last record incomplete or invalid: a crash mid-append.
                        // Normal; the caller truncates the file to valid_bytes.
    REPLAY_CORRUPT,     // an invalid record with valid data after it: real
                        // damage in the middle of the file. Not normal.
    REPLAY_NOMEM,
    REPLAY_IO,
} ReplayStatus;

typedef struct {
    ReplayStatus status;
    long valid_bytes;   // length of the prefix made of valid records
    long records;       // number of valid records applied
    long bad_offset;    // REPLAY_TORN_TAIL / REPLAY_CORRUPT: where it starts
} ReplayResult;

// Applies every valid record of the log at path to map, stopping at the
// first invalid one. A missing file is REPLAY_OK with 0 records.
ReplayResult replay_log(const char *path, HashMap *map);

const char *replay_status_str(ReplayStatus s);

#endif
