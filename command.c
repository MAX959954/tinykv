#include "command.h"
#include <string.h>
#include <stdio.h>

int parse_command(const char *line , Command * cmd){
    memset(cmd , 0 , sizeof(*cmd));
    cmd->type = CMD_UNKNOWN;

    char verb[16];
    int n = 0;
    if (sscanf(line , "%15s%n" , verb , &n) != 1) {
        return -1;
    }

    // Skip whitespace after the verb
    const char * rest = line + n;
    // Skip leading whitespace in the rest of the line
    while (*rest == ' ') rest++;
    
    if (strcmp(verb , "SET") == 0) {
        char key[128];
        // Use %n to get the number of characters consumed for the key
        int kn = 0 ;
        if (sscanf(rest , "%127s%n" , key , &kn) != 1){
            return -1;
        }
        // Skip whitespace after the key
        const char *value = rest + kn;
        // Skip leading whitespace in the value
        while(*value  == ' ') value++;
        
        if (*value == '\0') return -1;

        cmd->type = CMD_SET;
        // Copy key and value into the command structure, ensuring null-termination
        strncpy(cmd->key , key , sizeof(cmd->key) - 1 );
        strncpy(cmd->value , value , sizeof(cmd->value) - 1);
        return 0 ;
    }


    if (strcmp(verb , "GET") == 0 || strcmp(verb , "DEL") == 0) {
        char key[128];
        if (sscanf(rest  , "%127s" , key) != 1){
            return -1;
        }
        cmd->type = (verb[0] == 'G') ? CMD_GET : CMD_DEL;
        strncpy(cmd->key , key , sizeof(cmd->key) - 1 );
        return 0;
    }

    return -1;
};