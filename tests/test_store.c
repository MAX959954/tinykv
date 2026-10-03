#include "check.h"
#include "store.h"
#include "wal.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char dir[] = "/tmp/tinykv-test-store-XXXXXX";

static void path_in_dir(char *out, size_t n, const char *name) {
    snprintf(out, n, "%s/%s", dir, name);
}

static long file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static Store *open_mode(const char *path, StoreMode mode, uint64_t compact_bytes) {
    StoreOptions o = { mode, compact_bytes, 0 };
    return store_open(path, &o);
}

static int get_is(Store *s, const char *key, const char *want) {
    char out[MAX_LINE];
    StoreResult r = store_get(s, key, out, sizeof(out));
    if (!want) return r == STORE_NOT_FOUND;
    return r == STORE_OK && strcmp(out, want) == 0;
}

static void copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
}

// ---------------------------------------------------------------------------

static void test_basic_and_persistence(void) {
    char p[256]; path_in_dir(p, sizeof(p), "basic.log");
    Store *s = open_mode(p, STORE_MODE_GROUP, 0);
    CHECK(s != NULL);
    CHECK(get_is(s, "a", NULL));
    CHECK(store_set(s, "a", "1") == STORE_OK);
    CHECK(store_set(s, "b", "hello world") == STORE_OK);
    CHECK(store_set(s, "a", "2") == STORE_OK);
    CHECK(store_del(s, "b") == STORE_OK);
    CHECK(store_del(s, "b") == STORE_NOT_FOUND);
    CHECK(get_is(s, "a", "2"));
    store_close(s);

    s = open_mode(p, STORE_MODE_GROUP, 0);               // "restart"
    CHECK(get_is(s, "a", "2"));
    CHECK(get_is(s, "b", NULL));
    store_close(s);
}

static void test_get_truncates_to_buffer(void) {
    char p[256]; path_in_dir(p, sizeof(p), "trunc.log");
    char out[4];
    Store *s = open_mode(p, STORE_MODE_NOSYNC, 0);
    store_set(s, "k", "abcdef");
    CHECK(store_get(s, "k", out, sizeof(out)) == STORE_OK);
    CHECK_STR(out, "abc");
    store_close(s);
}

static void test_rejects_oversized_input(void) {
    char p[256]; path_in_dir(p, sizeof(p), "big.log");
    Store *s = open_mode(p, STORE_MODE_NOSYNC, 0);
    char key[MAX_KEY + 2], val[MAX_LINE + 1];
    memset(key, 'k', sizeof(key) - 1); key[sizeof(key) - 1] = '\0';
    memset(val, 'v', sizeof(val) - 1); val[sizeof(val) - 1] = '\0';
    CHECK(store_set(s, key, "v") == STORE_ERROR);
    CHECK(store_set(s, "k", val) == STORE_ERROR);
    store_close(s);
}

// A crash mid-append leaves a partial record at the end of the log. Without
// truncation, the next record would be glued onto it and both would be lost
// on the following replay.
static void test_torn_tail_is_truncated(void) {
    char p[256]; path_in_dir(p, sizeof(p), "torn.log");
    char rec[WAL_MAX_RECORD];
    size_t len = wal_format_record(rec, sizeof(rec), "SET a 1");
    FILE *f = fopen(p, "w");
    fwrite(rec, 1, len, f);
    fputs("0badc0de SET c 3", f);                        // no '\n'
    fclose(f);

    Store *s = open_mode(p, STORE_MODE_GROUP, 0);
    CHECK(s != NULL);
    CHECK(file_size(p) == (long)len);                    // tail cut off
    CHECK(get_is(s, "a", "1"));
    CHECK(get_is(s, "c", NULL));
    CHECK(store_set(s, "d", "4") == STORE_OK);
    store_close(s);

    s = open_mode(p, STORE_MODE_GROUP, 0);
    CHECK(get_is(s, "d", "4"));
    store_close(s);
}

static void test_corrupt_log_needs_repair(void) {
    char p[256]; path_in_dir(p, sizeof(p), "corrupt.log");
    char buf[2048] = "";
    const char *payloads[] = { "SET a 1", "SET b 2", "SET c 3" };
    size_t used = 0;
    for (int i = 0; i < 3; i++) {
        used += wal_format_record(buf + used, sizeof(buf) - used, payloads[i]);
    }
    long second = (long)(strchr(buf, '\n') - buf) + 1;
    buf[second + 15] = '9';                              // damage "SET b 2"
    FILE *f = fopen(p, "w");
    fputs(buf, f);
    fclose(f);

    Store *s = open_mode(p, STORE_MODE_GROUP, 0);
    CHECK(s == NULL);                                    // refuses: data after the damage
    CHECK(file_size(p) == (long)strlen(buf));            // and leaves the file alone

    StoreOptions o = { STORE_MODE_GROUP, 0, 1 };         // --repair
    s = store_open(p, &o);
    CHECK(s != NULL);
    if (s) {
        CHECK(get_is(s, "a", "1"));
        CHECK(get_is(s, "b", NULL));
        CHECK(get_is(s, "c", NULL));                     // lost, as announced
        CHECK(file_size(p) == second);
        store_close(s);
    }
}

// ---- compaction ----

static void test_manual_compaction(void) {
    char p[256], snap[300];
    path_in_dir(p, sizeof(p), "compact.log");
    snprintf(snap, sizeof(snap), "%s.snap", p);
    Store *s = open_mode(p, STORE_MODE_GROUP, 0);
    char k[16], v[16];
    for (int i = 0; i < 200; i++) {
        snprintf(k, sizeof(k), "k%d", i % 20);           // 20 keys, overwritten 10x
        snprintf(v, sizeof(v), "%d", i);
        store_set(s, k, v);
    }
    store_del(s, "k0");
    long before = file_size(p);
    CHECK(store_compact(s) == 0);
    CHECK(file_size(p) == 0);                            // log emptied
    CHECK(file_size(snap) > 0 && file_size(snap) < before);
    CHECK(store_stats(s).compactions == 1);
    CHECK(store_set(s, "after", "x") == STORE_OK);       // log keeps working
    store_close(s);

    s = open_mode(p, STORE_MODE_GROUP, 0);
    CHECK(store_stats(s).keys == 20);                    // k1..k19 + "after"
    CHECK(get_is(s, "k0", NULL));
    CHECK(get_is(s, "k19", "199"));
    CHECK(get_is(s, "after", "x"));
    store_close(s);
}

static void test_automatic_compaction_bounds_log(void) {
    char p[256]; path_in_dir(p, sizeof(p), "auto.log");
    Store *s = open_mode(p, STORE_MODE_NOSYNC, 4096);
    char k[16], v[64];
    for (int i = 0; i < 3000; i++) {
        snprintf(k, sizeof(k), "k%d", i % 10);
        snprintf(v, sizeof(v), "value-%d", i);
        store_set(s, k, v);
    }
    StoreStats st = store_stats(s);
    printf("      3000 SETs -> %llu compactions, log now %ld bytes\n",
           (unsigned long long)st.compactions, file_size(p));
    CHECK(st.compactions > 10);
    CHECK(file_size(p) < 4096 + 256 * WAL_MAX_RECORD);   // threshold + one batch
    store_close(s);

    s = open_mode(p, STORE_MODE_NOSYNC, 4096);
    CHECK(get_is(s, "k9", "value-2999"));
    CHECK(get_is(s, "k0", "value-2990"));
    store_close(s);
}

// A crash after the snapshot was renamed into place but before the log was
// truncated leaves the new snapshot plus the complete old log. Replaying the
// old log on top of the snapshot must give the same state.
static void test_crash_between_snapshot_and_truncate(void) {
    char p[256], saved[300];
    path_in_dir(p, sizeof(p), "mid.log");
    snprintf(saved, sizeof(saved), "%s.saved", p);
    Store *s = open_mode(p, STORE_MODE_GROUP, 0);
    store_set(s, "a", "1");
    store_set(s, "b", "1");
    store_del(s, "a");
    store_set(s, "b", "2");
    store_set(s, "c", "3");
    store_del(s, "c");
    store_set(s, "a", "final");
    copy_file(p, saved);                                 // the log as it was before compaction
    CHECK(store_compact(s) == 0);
    store_close(s);

    copy_file(saved, p);                                 // pretend the truncate never happened
    s = open_mode(p, STORE_MODE_GROUP, 0);
    CHECK(get_is(s, "a", "final"));
    CHECK(get_is(s, "b", "2"));
    CHECK(get_is(s, "c", NULL));
    CHECK(store_stats(s).keys == 2);
    store_close(s);
}

static void test_leftover_tmp_snapshot_ignored(void) {
    char p[256], tmp[320];
    path_in_dir(p, sizeof(p), "tmp.log");
    snprintf(tmp, sizeof(tmp), "%s.snap.tmp", p);
    Store *s = open_mode(p, STORE_MODE_GROUP, 0);
    store_set(s, "a", "1");
    store_close(s);
    FILE *f = fopen(tmp, "w");                           // crash while writing a snapshot
    fputs("half a snapsh", f);
    fclose(f);
    s = open_mode(p, STORE_MODE_GROUP, 0);
    CHECK(s != NULL);
    CHECK(get_is(s, "a", "1"));
    CHECK(access(tmp, F_OK) != 0);                       // cleaned up
    store_close(s);
}

// ---- async API ----

typedef struct {
    pthread_mutex_t mu;
    int calls;
    int ok;
} Counter;

static void count_done(void *ctx, StoreResult r) {
    Counter *c = ctx;
    pthread_mutex_lock(&c->mu);
    c->calls++;
    if (r == STORE_OK) c->ok++;
    pthread_mutex_unlock(&c->mu);
}

static void test_async_and_close_drains_queue(void) {
    char p[256]; path_in_dir(p, sizeof(p), "async.log");
    Store *s = open_mode(p, STORE_MODE_GROUP, 0);
    Counter c = { PTHREAD_MUTEX_INITIALIZER, 0, 0 };
    char k[16];
    int pending = 0;
    for (int i = 0; i < 500; i++) {
        snprintf(k, sizeof(k), "k%d", i);
        if (store_set_async(s, k, "v", count_done, &c) == STORE_PENDING) pending++;
    }
    CHECK(pending == 500);
    CHECK(store_del_async(s, "never-set", count_done, &c) == STORE_NOT_FOUND);  // immediate
    store_close(s);                                      // must commit all 500 first
    CHECK(c.calls == 500);
    CHECK(c.ok == 500);

    s = open_mode(p, STORE_MODE_GROUP, 0);
    CHECK(store_stats(s).keys == 500);
    store_close(s);
}

// ---- concurrency: memory must always match what the log replays to ----

#define THREADS 8
#define OPS 400
#define KEYS 8

static Store *shared;

static unsigned thread_ids[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

static void *worker(void *arg) {
    unsigned id = *(const unsigned *)arg;
    unsigned seed = id * 2654435761u;
    char key[16], val[32], out[64];
    for (int i = 0; i < OPS; i++) {
        seed = seed * 1103515245u + 12345u;
        unsigned r = (seed >> 16);
        snprintf(key, sizeof(key), "k%u", r % KEYS);     // few keys: lots of contention
        switch (r % 4) {
        case 0: case 1:
            snprintf(val, sizeof(val), "t%u-%d", id, i);
            store_set(shared, key, val);
            break;
        case 2:
            store_del(shared, key);
            break;
        default:
            store_get(shared, key, out, sizeof(out));
        }
    }
    return NULL;
}

static void run_consistency(StoreMode mode, uint64_t compact_bytes, const char *name) {
    char p[256]; path_in_dir(p, sizeof(p), name);
    shared = open_mode(p, mode, compact_bytes);
    CHECK(shared != NULL);
    pthread_t th[THREADS];
    for (int i = 0; i < THREADS; i++) pthread_create(&th[i], NULL, worker, &thread_ids[i]);
    for (int i = 0; i < THREADS; i++) pthread_join(th[i], NULL);

    // snapshot memory, then rebuild from snapshot + log and compare
    char before[KEYS][64], after[64], key[16];
    int present[KEYS];
    for (int k = 0; k < KEYS; k++) {
        snprintf(key, sizeof(key), "k%d", k);
        present[k] = store_get(shared, key, before[k], sizeof(before[k])) == STORE_OK;
    }
    StoreStats st = store_stats(shared);
    store_close(shared);

    Store *s = open_mode(p, mode, compact_bytes);
    int mismatches = 0;
    for (int k = 0; k < KEYS; k++) {
        snprintf(key, sizeof(key), "k%d", k);
        int now = store_get(s, key, after, sizeof(after)) == STORE_OK;
        if (now != present[k] || (now && strcmp(after, before[k]) != 0)) mismatches++;
    }
    store_close(s);
    printf("      %-14s: mismatches = %d, fsyncs = %llu, batches = %llu, compactions = %llu\n",
           name, mismatches, (unsigned long long)st.syncs, (unsigned long long)st.batches,
           (unsigned long long)st.compactions);
    CHECK(mismatches == 0);
}

static void test_concurrent_memory_matches_log(void) {
    run_consistency(STORE_MODE_GROUP, 0, "group");
    run_consistency(STORE_MODE_SERIAL, 0, "serial");
    run_consistency(STORE_MODE_NOSYNC, 0, "nosync");
    run_consistency(STORE_MODE_GROUP, 1024, "group+compact");  // compacting under load
}

#define WRITERS 8
#define PER_WRITER 200

static atomic_int writer_errors;

static void *distinct_writer(void *arg) {
    unsigned id = *(const unsigned *)arg;
    char key[32], val[32];
    for (int i = 0; i < PER_WRITER; i++) {
        snprintf(key, sizeof(key), "w%u-%d", id, i);
        snprintf(val, sizeof(val), "%d", i);
        if (store_set(shared, key, val) != STORE_OK) atomic_fetch_add(&writer_errors, 1);
    }
    return NULL;
}

static void test_group_commit_batches_fsyncs(void) {
    char p[256]; path_in_dir(p, sizeof(p), "batch.log");
    shared = open_mode(p, STORE_MODE_GROUP, 0);
    pthread_t th[WRITERS];
    for (int i = 0; i < WRITERS; i++) pthread_create(&th[i], NULL, distinct_writer, &thread_ids[i]);
    for (int i = 0; i < WRITERS; i++) pthread_join(th[i], NULL);
    CHECK(atomic_load(&writer_errors) == 0);
    StoreStats st = store_stats(shared);
    printf("      %d durable SETs from %d threads took %llu fsyncs\n",
           WRITERS * PER_WRITER, WRITERS, (unsigned long long)st.syncs);
    CHECK(st.syncs < (uint64_t)WRITERS * PER_WRITER);    // writes were batched
    store_close(shared);

    Store *s = open_mode(p, STORE_MODE_GROUP, 0);
    CHECK(store_stats(s).keys == (uint64_t)WRITERS * PER_WRITER);  // every acknowledged write survived
    store_close(s);
}

static void test_write_failure_leaves_memory_unchanged(void) {
    if (access("/dev/full", W_OK) != 0) {
        printf("      (skipped: no /dev/full)\n");
        return;
    }
    Store *s = open_mode("/dev/full", STORE_MODE_GROUP, 0);
    CHECK(s != NULL);
    CHECK(store_set(s, "a", "1") == STORE_ERROR);
    CHECK(get_is(s, "a", NULL));
    CHECK(store_set(s, "b", "2") == STORE_ERROR);        // stays read-only
    CHECK(store_compact(s) == -1);
    store_close(s);
}

int main(void) {
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    RUN(test_basic_and_persistence);
    RUN(test_get_truncates_to_buffer);
    RUN(test_rejects_oversized_input);
    RUN(test_torn_tail_is_truncated);
    RUN(test_corrupt_log_needs_repair);
    RUN(test_manual_compaction);
    RUN(test_automatic_compaction_bounds_log);
    RUN(test_crash_between_snapshot_and_truncate);
    RUN(test_leftover_tmp_snapshot_ignored);
    RUN(test_async_and_close_drains_queue);
    RUN(test_concurrent_memory_matches_log);
    RUN(test_group_commit_batches_fsyncs);
    RUN(test_write_failure_leaves_memory_unchanged);
    remove_tree(dir);
    return CHECK_DONE();
}
