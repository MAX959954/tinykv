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

void wal_write(FILE *wal_file, const char * record){
    fputs(record ,wal_file);
    fflush(wal_file);
    fsync(fileno(wal_file));
}

void replay_log(const char * path , HashMap *map){
    FILE * f = fopen(path , "r");
    if  (!f) return ;
    char line[MAX_LINE];
    while (fgets(line , sizeof(line) ,  f)){
        size_t len = strlen(line);
        if (len == 0 || line[len - 1] != '\n'){
            break;
        }
        line[len - 1] = '\0';

        Command cmd;

        if (parse_command(line , &cmd) != 0 ) continue;

        if(cmd.type == CMD_SET) hashmap_set(map , cmd.key , cmd.value);
        else if (cmd.type == CMD_DEL) hashmap_del(map , cmd.key);

    }
    fclose(f);
}

