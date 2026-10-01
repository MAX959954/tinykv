#include "linebuf.h"
#include <string.h>

void linebuf_init(LineBuf * lb){
    lb->len = 0 ;
}

size_t linebuf_space(const LineBuf * lb){
    return MAX_LINE - lb->len;
}

int linebuf_append(LineBuf * lb , const char * data , size_t len){
    if (len > linebuf_space(lb)){
        return -1;
    }
    memcpy(lb->buf + lb->len , data , len);
    lb->len += len;
    return 0;
}

/*
 * TCP is a byte stream, not a message stream: one recv() may return half a
 * command, or several pipelined commands at once. The buffer accumulates
 * bytes until a '\n' arrives, then hands out complete lines one at a time,
 * in order, keeping any trailing partial line for the next recv().
 */
int linebuf_extract(LineBuf * lb , char *line_out , size_t line_out_size){
    char * nl = memchr(lb->buf  , '\n' , lb->len);
    if (nl == NULL) {
        return 0;
    }

    size_t line_len = (size_t) (nl - lb->buf);
    size_t copy_len = line_len;
    if (copy_len >= line_out_size) {
        copy_len = line_out_size - 1;
    }
    memcpy(line_out , lb->buf , copy_len);
    line_out[copy_len] = '\0';

    size_t consumed = line_len + 1;   // the line plus its '\n'
    size_t remaining = lb->len - consumed;
    memmove(lb->buf , lb->buf + consumed , remaining);
    lb->len = remaining;

    return  1 ;
}
