#include "check.h"
#include "hashmap.h"

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

static void test_collisions(void) {
    // one bucket: every key lands in the same chain
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

int main(void) {
    RUN(test_set_get_overwrite_del);
    RUN(test_copies_its_inputs);
    RUN(test_collisions);
    RUN(test_many_keys);
    return CHECK_DONE();
}
