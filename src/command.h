#ifndef COMMAND_H
#define COMMAND_H
#include "linebuf.h"

// Max key length in bytes (not counting the terminating NUL).
// Longer keys are rejected with ERROR rather than silently truncated.
#define MAX_KEY 127

typedef enum {CMD_SET , CMD_GET , CMD_DEL , CMD_UNKNOWN} CommandType;

typedef struct {
    CommandType type;
    char key[MAX_KEY + 1];
    char value[MAX_LINE];
} Command;

//parse a command from a line, return 0 if successful, -1 if failed
int parse_command(const char *line , Command * cmd);

#endif
