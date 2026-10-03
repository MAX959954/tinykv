#include "hashmap.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct HashNode {
    char *key;
    char *value;
    uint64_t hash;            // cached, so resizing doesn't rehash the strings
    struct HashNode *next;
} HashNode;

struct HashMap {
    HashNode **buckets;
    size_t nbuckets;          // always a power of two: index = hash & (n - 1)
    size_t count;
};

// FNV-1a, 64-bit. (The previous djb2 with `% 1024` was fine for a fixed
// table; with power-of-two sizes and a mask, the low bits must be well mixed.)
static uint64_t hash_str(const char *key) {
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    return h;
}

static size_t round_up_pow2(size_t n) {
    size_t p = 8;
    while (p < n) p <<= 1;
    return p;
}

HashMap * hashmap_create(size_t initial_buckets) {
    HashMap *map = malloc(sizeof(HashMap));
    if (!map) return NULL;
    map->nbuckets = round_up_pow2(initial_buckets);
    map->buckets = (HashNode **)calloc(map->nbuckets, sizeof(HashNode *));
    if (!map->buckets) {
        free(map);
        return NULL;
    }
    map->count = 0;
    return map;
}

void hashmap_destroy(HashMap * map) {
    if (!map) return;
    for (size_t i = 0; i < map->nbuckets; i++) {
        HashNode *node = map->buckets[i];
        while (node) {
            HashNode *next = node->next;
            free(node->key);
            free(node->value);
            free(node);
            node = next;
        }
    }
    free((void *)map->buckets);
    free(map);
}

// Doubles the bucket array and moves every node into it. Nodes are relinked,
// not copied, so this can only fail on the one calloc -- in which case the
// map simply stays at its current size (still correct, just more collisions).
static void grow(HashMap *map) {
    size_t n = map->nbuckets * 2;
    HashNode **nb = (HashNode **)calloc(n, sizeof(HashNode *));
    if (!nb) return;
    for (size_t i = 0; i < map->nbuckets; i++) {
        HashNode *node = map->buckets[i];
        while (node) {
            HashNode *next = node->next;
            size_t idx = (size_t)(node->hash & (n - 1));
            node->next = nb[idx];
            nb[idx] = node;
            node = next;
        }
    }
    free((void *)map->buckets);
    map->buckets = nb;
    map->nbuckets = n;
}

static HashNode *find(const HashMap *map, const char *key, uint64_t h) {
    for (HashNode *node = map->buckets[h & (map->nbuckets - 1)]; node; node = node->next) {
        if (node->hash == h && strcmp(node->key, key) == 0) return node;
    }
    return NULL;
}

int hashmap_set(HashMap * map , const char * key , const char * value) {
    uint64_t h = hash_str(key);

    // Allocate everything first, then mutate: if an allocation fails the
    // map is left exactly as it was (never with a NULL value in a node).
    char *new_value = strdup(value);
    if (!new_value) return -1;

    HashNode *existing = find(map, key, h);
    if (existing) {
        free(existing->value);
        existing->value = new_value;
        return 0;
    }

    HashNode *node = malloc(sizeof(HashNode));
    char *new_key = strdup(key);
    if (!node || !new_key) {
        free(node);
        free(new_key);
        free(new_value);
        return -1;
    }
    node->key = new_key;
    node->value = new_value;
    node->hash = h;

    // load factor > 3/4 after this insert -> grow first
    if ((map->count + 1) * 4 > map->nbuckets * 3) grow(map);

    size_t idx = (size_t)(h & (map->nbuckets - 1));
    node->next = map->buckets[idx];
    map->buckets[idx] = node;
    map->count++;
    return 0;
}

const char * hashmap_get(const HashMap * map , const char * key) {
    HashNode *node = find(map, key, hash_str(key));
    return node ? node->value : NULL;
}

int hashmap_del(HashMap * map , const char * key) {
    uint64_t h = hash_str(key);
    HashNode **cur = &map->buckets[h & (map->nbuckets - 1)];
    while (*cur) {
        if ((*cur)->hash == h && strcmp((*cur)->key, key) == 0) {
            HashNode *dead = *cur;
            *cur = dead->next;
            free(dead->key);
            free(dead->value);
            free(dead);
            map->count--;
            return 0;
        }
        cur = &(*cur)->next;
    }
    return -1;
}

size_t hashmap_size(const HashMap * map) {
    return map->count;
}

size_t hashmap_buckets(const HashMap * map) {
    return map->nbuckets;
}

int hashmap_foreach(const HashMap * map ,
                    int (*fn)(const char *key , const char *value , void *ctx) ,
                    void *ctx) {
    for (size_t i = 0; i < map->nbuckets; i++) {
        for (const HashNode *node = map->buckets[i]; node; node = node->next) {
            int rc = fn(node->key, node->value, ctx);
            if (rc != 0) return rc;
        }
    }
    return 0;
}
