#ifndef HASHMAP_H
#define HASHMAP_H

#include <stddef.h>

typedef struct HashMap HashMap;

HashMap * hashmap_create(size_t nbuckets);
void hashmap_destroy(HashMap * map);
int hashmap_set(HashMap * map , const char * key , const char * value);
const char * hashmap_get(HashMap * map , const char * key);
int hashmap_del(HashMap * map , const char * key);

#endif 