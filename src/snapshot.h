#ifndef SNAPSHOT_H
#define SNAPSHOT_H
#include "hashmap.h"

// A snapshot is the whole key space written out as WAL-format SET records,
// followed by a trailer record "END <count>":
//
//     1c291ca3 SET a 1
//     9e1d3a2b SET b hello world
//     4f0ea8f1 END 2
//
// It lets the store truncate the WAL: startup loads the snapshot, then
// replays whatever the (now short) log contains on top of it.

typedef enum {
    SNAP_OK,
    SNAP_MISSING,    // no snapshot yet: fine, start from an empty map
    SNAP_CORRUPT,    // bad CRC, unparsable record, missing or wrong trailer
    SNAP_NOMEM,
    SNAP_IO,
} SnapStatus;

// Writes map to path atomically: a temporary file "<path>.tmp" is written
// and fsynced, renamed over path, and the directory is fsynced so the
// rename itself is durable. A crash at any point leaves either the old
// snapshot or the new one, never a mix. sync = 0 skips the fsyncs
// (benchmark mode). Returns 0 on success, -1 on failure (old file intact).
int snapshot_write(const char *path, const HashMap *map, int sync);

// Loads a snapshot into map; *entries receives the number of keys.
// Unlike the WAL, a snapshot never has a legitimately torn tail (it only
// appears via rename once complete), so any damage is SNAP_CORRUPT.
SnapStatus snapshot_load(const char *path, HashMap *map, long *entries);

const char *snapshot_status_str(SnapStatus s);

#endif
