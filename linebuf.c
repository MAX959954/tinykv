#include "linebuf.h"
#include <string.h>

void linebuf_init(LineBuf * lb){
    lb->len = 0 ;
}

int linebuf_append(LineBuf * lb , const char * data , size_t len){
    if (lb->len + len > MAX_LINE){
        return -1;
    }
    memcpy(lb->buf + lb->len , data , len);
    lb->len += len;
    return 0;
}

/*
the buffer may contain multiple lines, 
and we want to process them one by one.

why we want to process them one by one?
because we want to process them in the order they were received,
and we want to avoid blocking the processing of other lines while waiting 
for a complete line to be

why we want to avoid blocking the processing of other lines while waiting for
 a complete line to be received?
because we want to be able to process multiple lines concurrently, and we want
 to avoid blocking the processing of other lines while waiting for a complete 
line to be received.

why we need that for our server?
because our server is designed to handle multiple clients concurrently, and 
we want to be able to process 

*/

int linebuf_extract(LineBuf * lb , char *line_out , size_t line_out_size){
    char * nl = memchr(lb->buf  , '\n' , lb->len);
    if (nl == NULL) {
        return 0;
    }

    size_t line_len = (size_t) (nl - lb->buf);
    if (line_len >= line_out_size) {
        line_len = line_out_size - 1; 
    }
    memcpy(line_out , lb->buf , line_len);
    line_out[line_len] = '\0';

    size_t consumed = line_len + 1;
    consumed = (size_t) (nl - lb->buf) + 1;
    size_t remaining = lb->len - consumed;
    memmove(lb->buf , lb->buf + consumed , remaining);
    lb->len = remaining;

    return  1 ;
}