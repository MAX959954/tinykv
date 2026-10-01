#define _POSIX_C_SOURCE 200809L
#include "check.h"
#include "store.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

static char dir[] = "/tmp/tinykv-test-store-XXXXXX";

static void path_in_dir(char *out, size_t n, const char *name) {
    snprintf(out, n, "%s/%s", dir, name);
}

static void test_basic_and_persistence(void) {
    char p[256]; path_in_dir(p, sizeof(p), "basic.log");
    char out[64];
    Store *s = store_open(p, STORE_MODE_GROUP);
    CHECK(s != NULL);
    CHECK(store_get(s, "a", out, sizeof(out)) == STORE_NOT_FOUND);
    CHECK(store_set(s, "a", "1") == STORE_OK);
    CHECK(store_set(s, "b", "hello world") == STORE_OK);
    CHECK(store_set(s, "a", "2") == STORE_OK);
    CHECK(store_del(s, "b") == STORE_OK);
    CHECK(store_del(s, "b") == STORE_NOT_FOUND);
    CHECK(store_get(s, "a", out, sizeof(out)) == STORE_OK);
    CHECK_STR(out, "2");
    store_close(s);

    s = store_open(p, STORE_MODE_GROUP);                 // "restart"
    CHECK(store_get(s, "a", out, sizeof(out)) == STORE_OK);
    CHECK_STR(out, "2");
    CHECK(store_get(s, "b", out, sizeof(out)) == STORE_NOT_FOUND);
    store_close(s);
}

static void test_get_truncates_to_buffer(void) {
    char p[256]; path_in_dir(p, sizeof(p), "trunc.log");
    char out[4];
    Store *s = store_open(p, STORE_MODE_NOSYNC);
    store_set(s, "k", "abcdef");
    CHECK(store_get(s, "k", out, sizeof(out)) == STORE_OK);
    CHECK_STR(out, "abc");
    store_close(s);
}

// A crash mid-append leaves a partial record at the end of the log. Without
// truncation, the next record would be glued onto it ("SET c 3SET d 4\n")
// and both would be lost -- or worse, misparsed -- on the following replay.
static void test_torn_tail_is_truncated(void) {
    char p[256]; path_in_dir(p, sizeof(p), "torn.log");
    FILE *f = fopen(p, "w");
    fputs("SET a 1\nSET c 3", f);
    fclose(f);

    char out[64];
    Store *s = store_open(p, STORE_MODE_GROUP);
    CHECK(store_get(s, "a", out, sizeof(out)) == STORE_OK);
    CHECK(store_get(s, "c", out, sizeof(out)) == STORE_NOT_FOUND);
    CHECK(store_set(s, "d", "4") == STORE_OK);
    store_close(s);

    s = store_open(p, STORE_MODE_GROUP);
    CHECK(store_get(s, "d", out, sizeof(out)) == STORE_OK);
    CHECK_STR(out, "4");
    CHECK(store_get(s, "c", out, sizeof(out)) == STORE_NOT_FOUND);
    store_close(s);
}

// ---- concurrency: memory must always match what the log replays to ----

#define THREADS 8
#define OPS 400
#define KEYS 8

static Store *shared;

static void *worker(void *arg) {
    unsigned seed = (unsigned)(uintptr_t)arg * 2654435761u;
    char key[16], val[32], out[64];
    for (int i = 0; i < OPS; i++) {
        seed = seed * 1103515245u + 12345u;
        unsigned r = (seed >> 16);
        snprintf(key, sizeof(key), "k%u", r % KEYS);     // few keys: lots of contention
        switch (r % 4) {
        case 0: case 1:
            snprintf(val, sizeof(val), "t%u-%d", (unsigned)(uintptr_t)arg, i);
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

static void run_consistency(StoreMode mode, const char *name) {
    char p[256]; path_in_dir(p, sizeof(p), name);
    shared = store_open(p, mode);
    CHECK(shared != NULL);
    pthread_t th[THREADS];
    for (int i = 0; i < THREADS; i++) pthread_create(&th[i], NULL, worker, (void *)(uintptr_t)(i + 1));
    for (int i = 0; i < THREADS; i++) pthread_join(th[i], NULL);

    // snapshot memory, then rebuild from the log and compare
    char before[KEYS][64], after[64], key[16];
    int present[KEYS];
    for (int k = 0; k < KEYS; k++) {
        snprintf(key, sizeof(key), "k%d", k);
        present[k] = store_get(shared, key, before[k], sizeof(before[k])) == STORE_OK;
    }
    uint64_t syncs = store_sync_count(shared);
    store_close(shared);

    Store *s = store_open(p, mode);
    int mismatches = 0;
    for (int k = 0; k < KEYS; k++) {
        snprintf(key, sizeof(key), "k%d", k);
        int now = store_get(s, key, after, sizeof(after)) == STORE_OK;
        if (now != present[k] || (now && strcmp(after, before[k]) != 0)) mismatches++;
    }
    store_close(s);
    printf("      %-6s: memory vs replayed log mismatches = %d, fsyncs = %llu\n",
           name, mismatches, (unsigned long long)syncs);
    CHECK(mismatches == 0);
}

static void test_concurrent_memory_matches_log(void) {
    run_consistency(STORE_MODE_GROUP, "group");
    run_consistency(STORE_MODE_SERIAL, "serial");
    run_consistency(STORE_MODE_NOSYNC, "nosync");
}

#define WRITERS 8
#define PER_WRITER 200

static void *distinct_writer(void *arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    char key[32], val[32];
    for (int i = 0; i < PER_WRITER; i++) {
        snprintf(key, sizeof(key), "w%u-%d", id, i);
        snprintf(val, sizeof(val), "%d", i);
        if (store_set(shared, key, val) != STORE_OK) return (void *)1;
    }
    return NULL;
}

static void test_group_commit_batches_fsyncs(void) {
    char p[256]; path_in_dir(p, sizeof(p), "batch.log");
    shared = store_open(p, STORE_MODE_GROUP);
    pthread_t th[WRITERS];
    for (int i = 0; i < WRITERS; i++) pthread_create(&th[i], NULL, distinct_writer, (void *)(uintptr_t)i);
    int errors = 0;
    for (int i = 0; i < WRITERS; i++) {
        void *ret;
        pthread_join(th[i], &ret);
        if (ret) errors++;
    }
    CHECK(errors == 0);
    uint64_t syncs = store_sync_count(shared);
    printf("      %d durable SETs from %d threads took %llu fsyncs\n",
           WRITERS * PER_WRITER, WRITERS, (unsigned long long)syncs);
    CHECK(syncs < (uint64_t)(WRITERS * PER_WRITER));    // writes were batched
    store_close(shared);

    // every acknowledged write survives a reopen
    Store *s = store_open(p, STORE_MODE_GROUP);
    char key[32], out[32];
    int missing = 0;
    for (int t = 0; t < WRITERS; t++)
        for (int i = 0; i < PER_WRITER; i++) {
            snprintf(key, sizeof(key), "w%d-%d", t, i);
            if (store_get(s, key, out, sizeof(out)) != STORE_OK) missing++;
        }
    CHECK(missing == 0);
    store_close(s);
}

static void test_write_failure_leaves_memory_unchanged(void) {
    if (access("/dev/full", W_OK) != 0) {
        printf("      (skipped: no /dev/full)\n");
        return;
    }
    char out[16];
    Store *s = store_open("/dev/full", STORE_MODE_GROUP);
    CHECK(s != NULL);
    CHECK(store_set(s, "a", "1") == STORE_ERROR);
    CHECK(store_get(s, "a", out, sizeof(out)) == STORE_NOT_FOUND);
    CHECK(store_set(s, "b", "2") == STORE_ERROR);        // stays read-only
    store_close(s);
}

int main(void) {
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    RUN(test_basic_and_persistence);
    RUN(test_get_truncates_to_buffer);
    RUN(test_torn_tail_is_truncated);
    RUN(test_concurrent_memory_matches_log);
    RUN(test_group_commit_batches_fsyncs);
    RUN(test_write_failure_leaves_memory_unchanged);
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) fprintf(stderr, "warning: could not remove %s\n", dir);
    return CHECK_DONE();
}
