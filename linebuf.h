#ifndef LINEBUF_H
#define LINEBUF_H

#include <stddef.h> 

#define MAX_LINE 512

typedef struct {
    char buf[MAX_LINE];
    size_t len;
}LineBuf;

void linebuf_init(LineBuf * buf);

//append data to the line buffer, return the number of bytes appended
//return -1 if buffer would overflow
int linebuf_append(LineBuf * buf , const char * data , size_t len);

//extract a line from the line buffer, return 0 if a line was extracted, 
//-1 if no complete line is available
int linebuf_extract(LineBuf * buf , char *line_out , size_t line_out_size);


#endif 