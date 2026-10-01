#ifndef LINEBUF_H
#define LINEBUF_H

#include <stddef.h>

#define MAX_LINE 512

typedef struct {
    char buf[MAX_LINE];
    size_t len;
}LineBuf;

void linebuf_init(LineBuf * buf);

// Number of bytes that can still be appended before the buffer is full.
size_t linebuf_space(const LineBuf * buf);

// Append len bytes to the buffer.
// Returns 0 on success, -1 if they don't fit (nothing is appended).
// Callers should append at most linebuf_space() bytes at a time and
// drain complete lines with linebuf_extract() in between.
int linebuf_append(LineBuf * buf , const char * data , size_t len);

// Extract one complete '\n'-terminated line (without the '\n') into
// line_out. Returns 1 if a line was extracted, 0 if no complete line
// is buffered yet.
int linebuf_extract(LineBuf * buf , char *line_out , size_t line_out_size);


#endif
