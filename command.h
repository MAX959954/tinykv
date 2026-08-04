#ifndef COMMAND_H
#define COMMAND_H
#include "linebuf.h"

typedef enum {CMD_SET , CMD_GET , CMD_DEL , CMD_UNKNOWN} CommandType;

typedef struct {
    CommandType type;
    char key[128];
    char value[MAX_LINE];
} Command;

//parse a command from a line, return 0 if successful, -1 if failed
int parse_command(const char *line , Command * cmd);

#endif 
