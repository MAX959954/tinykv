#include "check.h"
#include "snapshot.h"
#include "wal.h"
#include <stdlib.h>
#include <unistd.h>

static char dir[] = "/tmp/tinykv-test-snap-XXXXXX";

static void path_in_dir(char *out, size_t n, const char *name) {
    snprintf(out, n, "%s/%s", dir, name);
}

static char *read_file(const char *path, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)*len + 1);
    *len = (long)fread(buf, 1, (size_t)*len, f);
    buf[*len] = '\0';
    fclose(f);
    return buf;
}

static void write_file(const char *path, const char *data, long len) {
    FILE *f = fopen(path, "wb");
    fwrite(data, 1, (size_t)len, f);
    fclose(f);
}

static HashMap *sample_map(int n) {
    HashMap *m = hashmap_create(8);
    char k[32], v[64];
    for (int i = 0; i < n; i++) {
        snprintf(k, sizeof(k), "key%d", i);
        snprintf(v, sizeof(v), "value %d with spaces", i * 3);
        hashmap_set(m, k, v);
    }
    return m;
}

static void test_roundtrip(void) {
    char p[256], tmp[300];
    path_in_dir(p, sizeof(p), "rt.snap");
    snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    HashMap *m = sample_map(1000);
    CHECK(snapshot_write(p, m, 1) == 0);
    CHECK(access(tmp, F_OK) != 0);                       // temp file renamed away

    HashMap *back = hashmap_create(8);
    long entries = -1;
    CHECK(snapshot_load(p, back, &entries) == SNAP_OK);
    CHECK(entries == 1000);
    CHECK(hashmap_size(back) == 1000);
    char k[32], v[64];
    int bad = 0;
    for (int i = 0; i < 1000; i++) {
        snprintf(k, sizeof(k), "key%d", i);
        snprintf(v, sizeof(v), "value %d with spaces", i * 3);
        const char *got = hashmap_get(back, k);
        if (!got || strcmp(got, v) != 0) bad++;
    }
    CHECK(bad == 0);
    hashmap_destroy(m);
    hashmap_destroy(back);
}

static void test_empty_map(void) {
    char p[256]; path_in_dir(p, sizeof(p), "empty.snap");
    HashMap *m = hashmap_create(8);
    CHECK(snapshot_write(p, m, 1) == 0);
    long entries = -1;
    CHECK(snapshot_load(p, m, &entries) == SNAP_OK);
    CHECK(entries == 0);
    hashmap_destroy(m);
}

static void test_missing(void) {
    char p[256]; path_in_dir(p, sizeof(p), "nope.snap");
    HashMap *m = hashmap_create(8);
    long entries;
    CHECK(snapshot_load(p, m, &entries) == SNAP_MISSING);
    hashmap_destroy(m);
}

static void test_overwrite_replaces(void) {
    char p[256]; path_in_dir(p, sizeof(p), "over.snap");
    HashMap *m = sample_map(50);
    CHECK(snapshot_write(p, m, 1) == 0);
    hashmap_destroy(m);
    m = sample_map(3);
    CHECK(snapshot_write(p, m, 1) == 0);
    hashmap_destroy(m);
    HashMap *back = hashmap_create(8);
    long entries;
    CHECK(snapshot_load(p, back, &entries) == SNAP_OK);
    CHECK(entries == 3);
    hashmap_destroy(back);
}

static void expect_corrupt(const char *name, const char *data, long len) {
    char p[256]; path_in_dir(p, sizeof(p), name);
    write_file(p, data, len);
    HashMap *m = hashmap_create(8);
    long entries;
    SnapStatus s = snapshot_load(p, m, &entries);
    if (s != SNAP_CORRUPT) fprintf(stderr, "      %s: got %s\n", name, snapshot_status_str(s));
    CHECK(s == SNAP_CORRUPT);
    hashmap_destroy(m);
}

static void test_damage_is_detected(void) {
    char p[256]; path_in_dir(p, sizeof(p), "good.snap");
    HashMap *m = sample_map(20);
    snapshot_write(p, m, 1);
    hashmap_destroy(m);
    long len = 0;
    char *good = read_file(p, &len);

    // cut off the trailer (and a bit more)
    expect_corrupt("truncated.snap", good, len - 20);
    // drop exactly the trailer line: all SET records valid, but no END
    char *last = good + len - 2;
    while (last > good && *last != '\n') last--;
    expect_corrupt("no-trailer.snap", good, (long)(last - good) + 1);
    // flip a byte in the middle
    char *flipped = malloc((size_t)len + 1);
    memcpy(flipped, good, (size_t)len + 1);
    flipped[len / 2] ^= 0x20;
    expect_corrupt("flipped.snap", flipped, len);
    // trailer count doesn't match the records: drop the first record
    char *first_nl = strchr(good, '\n');
    expect_corrupt("count.snap", first_nl + 1, len - (long)(first_nl + 1 - good));
    // garbage after the trailer
    char *extra = malloc((size_t)len + 32);
    memcpy(extra, good, (size_t)len);
    snprintf(extra + len, 32, "junk\n");
    expect_corrupt("extra.snap", extra, len + 5);

    free(good);
    free(flipped);
    free(extra);
}

int main(void) {
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    RUN(test_roundtrip);
    RUN(test_empty_map);
    RUN(test_missing);
    RUN(test_overwrite_replaces);
    RUN(test_damage_is_detected);
    remove_tree(dir);
    return CHECK_DONE();
}
