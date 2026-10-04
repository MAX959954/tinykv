#ifndef HASHMAP_H
#define HASHMAP_H

#include <stddef.h>

// Separate-chaining hash map from string keys to string values.
// Grows automatically: when the load factor (entries / buckets) would exceed
// 3/4, the bucket array is doubled and every entry is rehashed.
// Not thread-safe; the store guards it with a reader-writer lock.
typedef struct HashMap HashMap;

// initial_buckets is rounded up to a power of two (minimum 8).
// Returns NULL if allocation fails.
HashMap * hashmap_create(size_t initial_buckets);
void hashmap_destroy(HashMap * map);

// Inserts or overwrites key. Returns 0 on success, -1 if out of memory
// (in which case the map is unchanged).
int hashmap_set(HashMap * map , const char * key , const char * value);

// Returns the stored value, or NULL if key is absent. The pointer is owned
// by the map and is valid only until the next set/del of that key.
const char * hashmap_get(const HashMap * map , const char * key);

// Returns 0 if key was removed, -1 if it was not present.
int hashmap_del(HashMap * map , const char * key);

// Number of entries / current number of buckets.
size_t hashmap_size(const HashMap * map);
size_t hashmap_buckets(const HashMap * map);

// Calls fn for every entry (in no particular order) until fn returns
// non-zero, which is then returned. The map must not be modified meanwhile.
int hashmap_foreach(const HashMap * map ,
                    int (*fn)(const char *key , const char *value , void *ctx) ,
                    void *ctx);

#endif
