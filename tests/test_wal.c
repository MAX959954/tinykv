#define _POSIX_C_SOURCE 200809L
#include "check.h"
#include "wal.h"
#include <stdlib.h>
#include <unistd.h>

static char dir[] = "/tmp/tinykv-test-wal-XXXXXX";

static void path_in_dir(char *out, size_t n, const char *name) {
    snprintf(out, n, "%s/%s", dir, name);
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    fputs(content, f);
    fclose(f);
}

static void test_replay(void) {
    char p[256]; path_in_dir(p, sizeof(p), "replay.log");
    write_file(p, "SET a 1\nSET b two words\nSET a 3\nDEL b\nGARBAGE\nSET c 4\n");
    HashMap *m = hashmap_create(16);
    long valid = -1;
    CHECK(replay_log(p, m, &valid) == 0);
    CHECK_STR(hashmap_get(m, "a"), "3");
    CHECK(hashmap_get(m, "b") == NULL);
    CHECK_STR(hashmap_get(m, "c"), "4");                 // unparsable line skipped
    CHECK(valid == 54);                                  // whole file is valid
    hashmap_destroy(m);
}

static void test_replay_missing_file(void) {
    char p[256]; path_in_dir(p, sizeof(p), "does-not-exist.log");
    HashMap *m = hashmap_create(16);
    long valid = -1;
    CHECK(replay_log(p, m, &valid) == 0);
    CHECK(valid == 0);
    hashmap_destroy(m);
}

static void test_replay_stops_at_torn_tail(void) {
    char p[256]; path_in_dir(p, sizeof(p), "torn.log");
    write_file(p, "SET a 1\nSET b 2\nSET c 3");    // crash in the middle of the last append
    HashMap *m = hashmap_create(16);
    long valid = -1;
    CHECK(replay_log(p, m, &valid) == 0);
    CHECK_STR(hashmap_get(m, "a"), "1");
    CHECK_STR(hashmap_get(m, "b"), "2");
    CHECK(hashmap_get(m, "c") == NULL);
    CHECK(valid == 16);                                  // up to the end of "SET b 2\n"
    hashmap_destroy(m);
}

static void test_write_and_replay(void) {
    char p[256]; path_in_dir(p, sizeof(p), "append.log");
    Wal *w = wal_open(p, 1);
    CHECK(w != NULL);
    CHECK(wal_write(w, "SET x 1\n", 8) == 0);
    // a batch of several records is written (and fsynced) in one call
    CHECK(wal_write(w, "SET y 2\nSET z 3\nDEL x\n", 22) == 0);
    CHECK(wal_sync_count(w) == 2);
    CHECK(!wal_failed(w));
    wal_close(w);

    w = wal_open(p, 1);                                  // reopening appends
    CHECK(wal_write(w, "SET x 4\n", 8) == 0);
    wal_close(w);

    HashMap *m = hashmap_create(16);
    long valid;
    replay_log(p, m, &valid);
    CHECK_STR(hashmap_get(m, "x"), "4");
    CHECK_STR(hashmap_get(m, "y"), "2");
    CHECK_STR(hashmap_get(m, "z"), "3");
    CHECK(valid == 38);
    hashmap_destroy(m);
}

static void test_nosync_mode(void) {
    char p[256]; path_in_dir(p, sizeof(p), "nosync.log");
    Wal *w = wal_open(p, 0);
    CHECK(wal_write(w, "SET a 1\n", 8) == 0);
    CHECK(wal_sync_count(w) == 0);                       // never fsyncs
    wal_close(w);
}

static void test_write_failure_is_sticky(void) {
    if (access("/dev/full", W_OK) != 0) {
        printf("      (skipped: no /dev/full)\n");
        return;
    }
    Wal *w = wal_open("/dev/full", 1);                   // every write fails with ENOSPC
    CHECK(w != NULL);
    CHECK(wal_write(w, "SET a 1\n", 8) == -1);
    CHECK(wal_failed(w));
    CHECK(wal_write(w, "SET b 2\n", 8) == -1);          // stays failed
    wal_close(w);
}

int main(void) {
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    RUN(test_replay);
    RUN(test_replay_missing_file);
    RUN(test_replay_stops_at_torn_tail);
    RUN(test_write_and_replay);
    RUN(test_nosync_mode);
    RUN(test_write_failure_is_sticky);
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) fprintf(stderr, "warning: could not remove %s\n", dir);
    return CHECK_DONE();
}
