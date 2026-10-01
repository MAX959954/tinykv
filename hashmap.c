#define _POSIX_C_SOURCE 200809L
#include "hashmap.h"
#include <stdlib.h>
#include <string.h>

typedef struct HashNode {
    char * key;
    char *value;
    struct HashNode * next;
}HashNode;

struct HashMap {
    HashNode ** buckets;
    size_t nbuckets;
};

static unsigned long hash(const char * key) {
    unsigned long hash = 5381;
    while(*key){
        hash = ((hash << 5) + hash) + (unsigned char)(*key);
        key++;
    }
    return hash;
}

HashMap * hashmap_create(size_t nbuckets) {
    HashMap *map = malloc(sizeof(HashMap));
    if (!map) return NULL;
    map->buckets = calloc(nbuckets, sizeof(HashNode*));
    if (!map->buckets) {
        free(map);
        return NULL;
    }
    map->nbuckets = nbuckets;
    return map ;
}

void hashmap_destroy(HashMap * map){
    for(size_t i = 0 ; i < map->nbuckets ; i++) {
        HashNode * node = map->buckets[i];

        while(node) {
            HashNode * next = node->next;
            free(node->key);
            free(node->value);
            free(node);
            node = next;
        }
    }
    free(map->buckets);
    free(map);
}

int hashmap_set(HashMap * map , const char * key , const char * value){
    size_t index = hash(key) % map->nbuckets;

    // Allocate everything first, then mutate: if an allocation fails the
    // map is left exactly as it was (never with a NULL value in a node).
    char *new_value = strdup(value);
    if (!new_value) return -1;

    for (HashNode * node = map->buckets[index]; node ; node = node->next) {
        if (strcmp(node->key ,key) == 0) {
            free(node->value);
            node->value = new_value;
            return 0;
        }
    }

    HashNode * new_node = malloc(sizeof(HashNode));
    char *new_key = strdup(key);
    if (!new_node || !new_key) {
        free(new_node);
        free(new_key);
        free(new_value);
        return -1;
    }
    new_node->key = new_key;
    new_node->value = new_value;
    new_node->next = map->buckets[index];
    map->buckets[index] = new_node;
    return 0;
}

const char * hashmap_get(HashMap * map , const char * key){
    size_t index  = hash(key) % map->nbuckets;
    for  (HashNode * node = map->buckets[index]; node ; node = node->next) {
        if (strcmp(node->key , key) == 0){
            return node->value;
        }
    }
    return NULL;
}

int hashmap_del(HashMap * map , const char * key){
    size_t index = hash(key) % map->nbuckets;
    HashNode **cur = &map->buckets[index];
    while (*cur) {
        if (strcmp((*cur)->key , key) == 0) {
            HashNode * dead  = *cur;
            *cur = dead->next;
            free(dead->key);
            free(dead->value);
            free(dead);
            return 0;
        }
        cur = (&(*cur)->next);
    }

    return -1;
}
