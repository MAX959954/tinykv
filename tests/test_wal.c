#include "check.h"
#include "crc32.h"
#include "wal.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char dir[] = "/tmp/tinykv-test-wal-XXXXXX";

static void path_in_dir(char *out, size_t n, const char *name) {
    snprintf(out, n, "%s/%s", dir, name);
}

// Appends the record for payload to the string in buf (capacity cap).
static void add_record(char *buf, size_t cap, const char *payload) {
    size_t len = strlen(buf);
    wal_format_record(buf + len, cap - len, payload);
}
#define add(buf, payload) add_record(buf, sizeof(buf), payload)

static void write_bytes(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    fwrite(data, 1, len, f);
    fclose(f);
}

static long file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static void test_crc32_known_values(void) {
    CHECK(crc32("123456789", 9) == 0xCBF43926u);         // the standard check value
    CHECK(crc32("", 0) == 0);
    CHECK(crc32("a", 1) == 0xE8B7BE43u);
}

static void test_format_and_parse(void) {
    char rec[WAL_MAX_RECORD];
    size_t len = wal_format_record(rec, sizeof(rec), "SET user:1 alice smith");
    CHECK(len == strlen(rec));
    CHECK(rec[len - 1] == '\n');
    CHECK(rec[8] == ' ');

    Command c;
    CHECK(wal_parse_record(rec, len - 1, &c) == 0);
    CHECK(c.type == CMD_SET);
    CHECK_STR(c.key, "user:1");
    CHECK_STR(c.value, "alice smith");

    // any single flipped character is caught
    int caught = 0;
    for (size_t i = 0; i < len - 1; i++) {
        char bad[WAL_MAX_RECORD];
        memcpy(bad, rec, len);
        bad[i] ^= 0x01;
        if (wal_parse_record(bad, len - 1, &c) != 0) caught++;
    }
    CHECK(caught == (int)(len - 1));

    CHECK(wal_parse_record("deadbeef GET x", 14, &c) == -1);   // not a write
    CHECK(wal_parse_record("SET a 1", 7, &c) == -1);            // no checksum (old format)
    CHECK(wal_format_record(rec, 10, "SET a 1") == 0);          // doesn't fit
}

static void test_replay_ok(void) {
    char p[256]; path_in_dir(p, sizeof(p), "ok.log");
    char buf[4096] = "";
    add(buf, "SET a 1");
    add(buf, "SET b two words");
    add(buf, "SET a 3");
    add(buf, "DEL b");
    write_bytes(p, buf, strlen(buf));

    HashMap *m = hashmap_create(16);
    ReplayResult r = replay_log(p, m);
    CHECK(r.status == REPLAY_OK);
    CHECK(r.records == 4);
    CHECK(r.valid_bytes == (long)strlen(buf));
    CHECK_STR(hashmap_get(m, "a"), "3");
    CHECK(hashmap_get(m, "b") == NULL);
    hashmap_destroy(m);
}

static void test_replay_missing_file(void) {
    char p[256]; path_in_dir(p, sizeof(p), "does-not-exist.log");
    HashMap *m = hashmap_create(16);
    ReplayResult r = replay_log(p, m);
    CHECK(r.status == REPLAY_OK);
    CHECK(r.records == 0 && r.valid_bytes == 0);
    hashmap_destroy(m);
}

static void test_torn_tail_partial_record(void) {
    char p[256]; path_in_dir(p, sizeof(p), "torn.log");
    char buf[4096] = "";
    add(buf, "SET a 1");
    add(buf, "SET b 2");
    long good = (long)strlen(buf);
    snprintf(buf + good, sizeof(buf) - (size_t)good, "1234abcd SET c 3");  // crash mid-append: no '\n'
    write_bytes(p, buf, strlen(buf));

    HashMap *m = hashmap_create(16);
    ReplayResult r = replay_log(p, m);
    CHECK(r.status == REPLAY_TORN_TAIL);
    CHECK(r.valid_bytes == good && r.bad_offset == good);
    CHECK(r.records == 2);
    CHECK(hashmap_get(m, "c") == NULL);
    hashmap_destroy(m);
}

static void test_torn_tail_zero_filled(void) {
    // some filesystems can leave a zero-filled block after a crash
    char p[256]; path_in_dir(p, sizeof(p), "zeros.log");
    char buf[8192] = "";
    add(buf, "SET a 1");
    size_t good = strlen(buf);
    memset(buf + good, 0, 4096);
    write_bytes(p, buf, good + 4096);

    HashMap *m = hashmap_create(16);
    ReplayResult r = replay_log(p, m);
    CHECK(r.status == REPLAY_TORN_TAIL);
    CHECK(r.valid_bytes == (long)good);
    CHECK_STR(hashmap_get(m, "a"), "1");
    hashmap_destroy(m);
}

static void test_corruption_in_the_middle(void) {
    char p[256]; path_in_dir(p, sizeof(p), "corrupt.log");
    char buf[4096] = "";
    add(buf, "SET a 1");
    long bad_at = (long)strlen(buf);
    add(buf, "SET b 2");
    add(buf, "SET c 3");
    add(buf, "SET d 4");
    buf[bad_at + 14] = 'X';                              // damage the "SET b 2" record
    write_bytes(p, buf, strlen(buf));

    HashMap *m = hashmap_create(16);
    ReplayResult r = replay_log(p, m);
    // valid records follow the bad one: this is damage, not a torn write
    CHECK(r.status == REPLAY_CORRUPT);
    CHECK(r.bad_offset == bad_at);
    CHECK(r.valid_bytes == bad_at);
    CHECK(r.records == 1);
    CHECK_STR(hashmap_get(m, "a"), "1");
    CHECK(hashmap_get(m, "b") == NULL);
    CHECK(hashmap_get(m, "d") == NULL);                  // nothing past the damage
    hashmap_destroy(m);
}

static void test_bad_last_complete_record_is_torn(void) {
    char p[256]; path_in_dir(p, sizeof(p), "badlast.log");
    char buf[4096] = "";
    add(buf, "SET a 1");
    long good = (long)strlen(buf);
    add(buf, "SET b 2");
    buf[good + 12] = 'Z';
    write_bytes(p, buf, strlen(buf));

    HashMap *m = hashmap_create(16);
    ReplayResult r = replay_log(p, m);
    CHECK(r.status == REPLAY_TORN_TAIL);                 // nothing after it
    CHECK(r.valid_bytes == good);
    hashmap_destroy(m);
}

static void test_write_truncate_replay(void) {
    char p[256]; path_in_dir(p, sizeof(p), "append.log");
    char buf[4096] = "";
    Wal *w = wal_open(p, 1);
    CHECK(w != NULL);
    add(buf, "SET x 1");
    CHECK(wal_write(w, buf, strlen(buf)) == 0);
    buf[0] = '\0';
    add(buf, "SET y 2");                                 // a batch of several records
    add(buf, "DEL x");
    CHECK(wal_write(w, buf, strlen(buf)) == 0);
    CHECK(wal_sync_count(w) == 2);
    CHECK(wal_size(w) == (uint64_t)file_size(p));

    CHECK(wal_truncate(w) == 0);
    CHECK(wal_size(w) == 0 && file_size(p) == 0);
    buf[0] = '\0';
    add(buf, "SET z 3");
    CHECK(wal_write(w, buf, strlen(buf)) == 0);          // O_APPEND: lands at offset 0
    CHECK(file_size(p) == (long)strlen(buf));
    wal_close(w);

    HashMap *m = hashmap_create(16);
    ReplayResult r = replay_log(p, m);
    CHECK(r.status == REPLAY_OK && r.records == 1);
    CHECK_STR(hashmap_get(m, "z"), "3");
    hashmap_destroy(m);
}

static void test_nosync_mode(void) {
    char p[256]; path_in_dir(p, sizeof(p), "nosync.log");
    char buf[256] = "";
    add(buf, "SET a 1");
    Wal *w = wal_open(p, 0);
    CHECK(wal_write(w, buf, strlen(buf)) == 0);
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
    CHECK(wal_write(w, "x\n", 2) == -1);
    CHECK(wal_failed(w));
    CHECK(wal_write(w, "y\n", 2) == -1);                 // stays failed
    CHECK(wal_truncate(w) == -1);
    wal_close(w);
}

int main(void) {
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    RUN(test_crc32_known_values);
    RUN(test_format_and_parse);
    RUN(test_replay_ok);
    RUN(test_replay_missing_file);
    RUN(test_torn_tail_partial_record);
    RUN(test_torn_tail_zero_filled);
    RUN(test_corruption_in_the_middle);
    RUN(test_bad_last_complete_record_is_torn);
    RUN(test_write_truncate_replay);
    RUN(test_nosync_mode);
    RUN(test_write_failure_is_sticky);
    remove_tree(dir);
    return CHECK_DONE();
}
