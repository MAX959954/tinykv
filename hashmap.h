#ifndef HASHMAP_H
#define HASHMAP_H

#include <stddef.h>

typedef struct HashMap HashMap;

// Returns NULL if allocation fails.
HashMap * hashmap_create(size_t nbuckets);
void hashmap_destroy(HashMap * map);

// Inserts or overwrites key. Returns 0 on success, -1 if out of memory
// (in which case the map is unchanged).
int hashmap_set(HashMap * map , const char * key , const char * value);

// Returns the stored value, or NULL if key is absent. The pointer is owned
// by the map and is valid only until the next set/del of that key.
const char * hashmap_get(HashMap * map , const char * key);

// Returns 0 if key was removed, -1 if it was not present.
int hashmap_del(HashMap * map , const char * key);

#endif
