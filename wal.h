#ifndef WAL_H
#define WAL_H
#include <stdio.h>
#include "hashmap.h"
#include "command.h"

// Largest possible WAL record: "SET " + key + " " + value + "\n" + NUL.
// Both the writer (dispatch) and the reader (replay_log) size their
// buffers from this, so a record that was accepted can always be replayed.
#define WAL_MAX_RECORD (4 + MAX_KEY + 1 + MAX_LINE + 1 + 1)

FILE  * wal_open(const char * path);
void wal_write(FILE *wal_file, const char * record);
void replay_log(const char * path , HashMap *map);

#endif
