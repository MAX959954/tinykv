#include "check.h"
#include "hashmap.h"
#include <stdlib.h>

static void test_set_get_overwrite_del(void) {
    HashMap *m = hashmap_create(16);
    CHECK(m != NULL);
    CHECK(hashmap_get(m, "a") == NULL);
    CHECK(hashmap_set(m, "a", "1") == 0);
    CHECK_STR(hashmap_get(m, "a"), "1");
    CHECK(hashmap_set(m, "a", "2") == 0);                // overwrite
    CHECK_STR(hashmap_get(m, "a"), "2");
    CHECK(hashmap_del(m, "a") == 0);
    CHECK(hashmap_get(m, "a") == NULL);
    CHECK(hashmap_del(m, "a") == -1);                    // already gone
    hashmap_destroy(m);
}

static void test_copies_its_inputs(void) {
    HashMap *m = hashmap_create(16);
    char key[8] = "key", val[8] = "val";
    hashmap_set(m, key, val);
    strcpy(key, "zzz");
    strcpy(val, "XXX");
    CHECK_STR(hashmap_get(m, "key"), "val");
    hashmap_destroy(m);
}

static void test_deletes_across_resizes(void) {
    // starts at the minimum size, so it resizes several times on the way
    HashMap *m = hashmap_create(1);
    char k[16], v[16];
    for (int i = 0; i < 100; i++) {
        snprintf(k, sizeof(k), "k%d", i);
        snprintf(v, sizeof(v), "v%d", i);
        CHECK(hashmap_set(m, k, v) == 0);
    }
    // delete from head, middle and tail of the chain
    CHECK(hashmap_del(m, "k99") == 0);
    CHECK(hashmap_del(m, "k50") == 0);
    CHECK(hashmap_del(m, "k0") == 0);
    for (int i = 0; i < 100; i++) {
        snprintf(k, sizeof(k), "k%d", i);
        snprintf(v, sizeof(v), "v%d", i);
        if (i == 0 || i == 50 || i == 99) CHECK(hashmap_get(m, k) == NULL);
        else CHECK_STR(hashmap_get(m, k), v);
    }
    hashmap_destroy(m);
}

static void test_many_keys(void) {
    HashMap *m = hashmap_create(64);
    char k[32], v[32];
    for (int i = 0; i < 10000; i++) {
        snprintf(k, sizeof(k), "key-%d", i);
        snprintf(v, sizeof(v), "value-%d", i * 7);
        hashmap_set(m, k, v);
    }
    int bad = 0;
    for (int i = 0; i < 10000; i++) {
        snprintf(k, sizeof(k), "key-%d", i);
        snprintf(v, sizeof(v), "value-%d", i * 7);
        const char *got = hashmap_get(m, k);
        if (!got || strcmp(got, v) != 0) bad++;
    }
    CHECK(bad == 0);
    hashmap_destroy(m);
}

static void test_growth_and_size(void) {
    HashMap *m = hashmap_create(8);
    CHECK(hashmap_buckets(m) == 8);
    char k[32];
    for (int i = 0; i < 1000; i++) {
        snprintf(k, sizeof(k), "k%d", i);
        hashmap_set(m, k, "v");
        // the load factor never exceeds 3/4
        CHECK(hashmap_size(m) * 4 <= hashmap_buckets(m) * 3);
    }
    CHECK(hashmap_size(m) == 1000);
    CHECK(hashmap_buckets(m) == 2048);                   // 8 -> ... -> 2048
    hashmap_set(m, "k5", "overwrite");                   // overwrite: size unchanged
    CHECK(hashmap_size(m) == 1000);
    hashmap_del(m, "k5");
    hashmap_del(m, "missing");
    CHECK(hashmap_size(m) == 999);
    hashmap_destroy(m);
}

static int count_entries(const char *key, const char *value, void *ctx) {
    (void)value;
    int *seen = ctx;
    int i = (int)strtol(key + 1, NULL, 10);              // keys are "k<i>"
    if (i >= 0 && i < 500) seen[i]++;
    return 0;
}

static int stop_at_three(const char *key, const char *value, void *ctx) {
    (void)key; (void)value;
    int *n = ctx;
    return ++*n == 3 ? 42 : 0;
}

static void test_foreach(void) {
    HashMap *m = hashmap_create(8);
    char k[16];
    for (int i = 0; i < 500; i++) {
        snprintf(k, sizeof(k), "k%d", i);
        hashmap_set(m, k, "v");
    }
    int seen[500] = {0};
    CHECK(hashmap_foreach(m, count_entries, seen) == 0);
    int bad = 0;
    for (int i = 0; i < 500; i++) if (seen[i] != 1) bad++;
    CHECK(bad == 0);                                     // every entry exactly once

    int n = 0;
    CHECK(hashmap_foreach(m, stop_at_three, &n) == 42);  // early stop propagates
    CHECK(n == 3);
    hashmap_destroy(m);
}

int main(void) {
    RUN(test_set_get_overwrite_del);
    RUN(test_copies_its_inputs);
    RUN(test_deletes_across_resizes);
    RUN(test_many_keys);
    RUN(test_growth_and_size);
    RUN(test_foreach);
    return CHECK_DONE();
}
