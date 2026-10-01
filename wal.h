#ifndef WAL_H
#define WAL_H
#include <stdio.h>
#include "hashmap.h"
#include "command.h"

// Largest possible WAL record: "SET " + key + " " + value + "\n" + NUL.
// Both the writer (dispatch) and the reader (replay_log) size their
// buffers from this, so a record that was accepted can always be replayed.
#define WAL_MAX_RECORD (4 + MAX_KEY + 1 + MAX_LINE + 1 + 1)

// Opens the log for appending. Exits the process if it can't be opened.
FILE  * wal_open(const char * path);

// Appends one record and makes it durable (fputs -> fflush -> fsync).
// Returns 0 only once the record is on disk, -1 on any I/O error.
int wal_write(FILE *wal_file, const char * record);

// Rebuilds map from the log. A missing log is not an error.
// Returns 0 on success, -1 if the map ran out of memory.
int replay_log(const char * path , HashMap *map);

#endif
