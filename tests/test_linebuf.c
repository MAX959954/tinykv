#include "check.h"
#include "linebuf.h"

static int feed(LineBuf *lb, const char *s) {
    return linebuf_append(lb, s, strlen(s));
}

static void test_single_line(void) {
    LineBuf lb; char out[MAX_LINE];
    linebuf_init(&lb);
    CHECK(feed(&lb, "GET a\n") == 0);
    CHECK(linebuf_extract(&lb, out, sizeof(out)) == 1);
    CHECK_STR(out, "GET a");
    CHECK(linebuf_extract(&lb, out, sizeof(out)) == 0);
    CHECK(lb.len == 0);
}

static void test_partial_line(void) {
    LineBuf lb; char out[MAX_LINE];
    linebuf_init(&lb);
    feed(&lb, "SET k ");
    CHECK(linebuf_extract(&lb, out, sizeof(out)) == 0);   // not complete yet
    feed(&lb, "val");
    CHECK(linebuf_extract(&lb, out, sizeof(out)) == 0);
    feed(&lb, "ue\n");
    CHECK(linebuf_extract(&lb, out, sizeof(out)) == 1);
    CHECK_STR(out, "SET k value");
}

static void test_pipelined_lines(void) {
    LineBuf lb; char out[MAX_LINE];
    linebuf_init(&lb);
    feed(&lb, "A\nB\nC");
    CHECK(linebuf_extract(&lb, out, sizeof(out)) == 1); CHECK_STR(out, "A");
    CHECK(linebuf_extract(&lb, out, sizeof(out)) == 1); CHECK_STR(out, "B");
    CHECK(linebuf_extract(&lb, out, sizeof(out)) == 0);   // "C" still partial
    feed(&lb, "\n");
    CHECK(linebuf_extract(&lb, out, sizeof(out)) == 1); CHECK_STR(out, "C");
}

static void test_crlf(void) {
    LineBuf lb; char out[MAX_LINE];
    linebuf_init(&lb);
    feed(&lb, "GET a\r\nGET b\n\r\n");
    CHECK(linebuf_extract(&lb, out, sizeof(out)) == 1); CHECK_STR(out, "GET a");
    CHECK(linebuf_extract(&lb, out, sizeof(out)) == 1); CHECK_STR(out, "GET b");
    CHECK(linebuf_extract(&lb, out, sizeof(out)) == 1); CHECK_STR(out, "");
}

static void test_capacity(void) {
    LineBuf lb;
    char big[MAX_LINE + 1];
    memset(big, 'x', sizeof(big));
    linebuf_init(&lb);
    CHECK(linebuf_space(&lb) == MAX_LINE);
    CHECK(linebuf_append(&lb, big, MAX_LINE + 1) == -1);  // too much: nothing appended
    CHECK(lb.len == 0);
    CHECK(linebuf_append(&lb, big, MAX_LINE) == 0);       // exactly full is fine
    CHECK(linebuf_space(&lb) == 0);
    CHECK(linebuf_append(&lb, "y", 1) == -1);
}

static void test_space_frees_after_extract(void) {
    LineBuf lb; char out[MAX_LINE];
    linebuf_init(&lb);
    feed(&lb, "0123456789\nrest");
    CHECK(linebuf_space(&lb) == MAX_LINE - 15);
    linebuf_extract(&lb, out, sizeof(out));
    CHECK(linebuf_space(&lb) == MAX_LINE - 4);            // only "rest" remains
}

static void test_small_output_buffer_truncates_but_consumes(void) {
    LineBuf lb; char out[4];
    linebuf_init(&lb);
    feed(&lb, "abcdef\nX\n");
    CHECK(linebuf_extract(&lb, out, sizeof(out)) == 1);
    CHECK_STR(out, "abc");
    CHECK(linebuf_extract(&lb, out, sizeof(out)) == 1);   // whole first line consumed
    CHECK_STR(out, "X");
}

int main(void) {
    RUN(test_single_line);
    RUN(test_partial_line);
    RUN(test_pipelined_lines);
    RUN(test_crlf);
    RUN(test_capacity);
    RUN(test_space_frees_after_extract);
    RUN(test_small_output_buffer_truncates_but_consumes);
    return CHECK_DONE();
}
