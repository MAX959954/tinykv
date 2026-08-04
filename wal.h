#ifndef WAL_H 
#define WAL_H 
#include <stdio.h>
#include "hashmap.h"

FILE  * wal_open(const char * path);
void wal_write(FILE *wal_file, const char * record);
void replay_log(const char * path , HashMap *map);

#endif 

