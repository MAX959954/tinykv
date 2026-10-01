#include "wal.h"
#include "command.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

FILE  * wal_open(const char * path) {
    FILE * f = fopen(path , "a");
    if (!f) {
        perror("wal_open");
        exit (1);
    }
    return f;
}

int wal_write(FILE *wal_file, const char * record){
    if (fputs(record , wal_file) == EOF) return -1;
    if (fflush(wal_file) != 0) return -1;
    if (fsync(fileno(wal_file)) != 0) return -1;
    return 0;
}

int replay_log(const char * path , HashMap *map){
    FILE * f = fopen(path , "r");
    if  (!f) return 0;
    char line[WAL_MAX_RECORD];
    while (fgets(line , sizeof(line) ,  f)){
        size_t len = strlen(line);
        // A record without a trailing '\n' can only be a torn write at the
        // very end of the log (crash mid-append) — stop replaying there.
        if (len == 0 || line[len - 1] != '\n'){
            break;
        }
        line[len - 1] = '\0';

        Command cmd;

        if (parse_command(line , &cmd) != 0 ) continue;

        if (cmd.type == CMD_SET) {
            if (hashmap_set(map , cmd.key , cmd.value) != 0) {
                fclose(f);
                return -1;
            }
        } else if (cmd.type == CMD_DEL) {
            hashmap_del(map , cmd.key);
        }
    }
    fclose(f);
    return 0;
}
