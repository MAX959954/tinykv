#include "command.h"
#include <string.h>

static const char *skip_spaces(const char *s) {
    while (*s == ' ') s++;
    return s;
}

// Copies the next space-delimited token from *src into out (size out_size)
// and advances *src past it. Returns -1 if the token is empty or does not
// fit — we never truncate, because a truncated key silently becomes a
// different key (and the leftover bytes would leak into the value).
static int take_token(const char **src , char *out , size_t out_size) {
    const char *s = *src;
    size_t len = strcspn(s , " ");
    if (len == 0 || len >= out_size) return -1;
    memcpy(out , s , len);
    out[len] = '\0';
    *src = s + len;
    return 0;
}

int parse_command(const char *line , Command * cmd){
    memset(cmd , 0 , sizeof(*cmd));
    cmd->type = CMD_UNKNOWN;

    const char *p = skip_spaces(line);

    char verb[8];
    if (take_token(&p , verb , sizeof(verb)) != 0) return -1;
    p = skip_spaces(p);

    if (strcmp(verb , "SET") == 0) {
        if (take_token(&p , cmd->key , sizeof(cmd->key)) != 0) return -1;

        // value = everything after the key, so it may contain spaces
        const char *value = skip_spaces(p);
        size_t vlen = strlen(value);
        if (vlen == 0 || vlen >= sizeof(cmd->value)) return -1;
        memcpy(cmd->value , value , vlen + 1);

        cmd->type = CMD_SET;
        return 0;
    }

    if (strcmp(verb , "GET") == 0 || strcmp(verb , "DEL") == 0) {
        if (take_token(&p , cmd->key , sizeof(cmd->key)) != 0) return -1;
        cmd->type = (verb[0] == 'G') ? CMD_GET : CMD_DEL;
        return 0;
    }

    return -1;
}
