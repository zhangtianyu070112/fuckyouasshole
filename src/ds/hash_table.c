/**
 * @file    hash_table.c
 * @brief   Generic hash table implementation (string keys, separate chaining).
 */

#include "hash_table.h"
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 *  DJB2 hash function
 * ========================================================================= */

uint32_t ht_hash_string(const char* key)
{
    uint32_t hash = 5381;
    int c;
    while ((c = (unsigned char)*key++)) {
        hash = ((hash << 5) + hash) + (uint32_t)c;  /* hash * 33 + c */
    }
    return hash;
}

/* =========================================================================
 *  Lifecycle
 * ========================================================================= */

HashTable* ht_create(int bucket_count)
{
    if (bucket_count <= 0) bucket_count = 211;

    HashTable* ht = calloc(1, sizeof(HashTable));
    if (!ht) return NULL;

    ht->buckets = calloc((size_t)bucket_count, sizeof(HTEntry*));
    if (!ht->buckets) {
        free(ht);
        return NULL;
    }

    ht->bucket_count = bucket_count;
    ht->size = 0;
    return ht;
}

void ht_destroy(HashTable* ht, int free_values)
{
    if (!ht) return;

    for (int i = 0; i < ht->bucket_count; i++) {
        HTEntry* entry = ht->buckets[i];
        while (entry) {
            HTEntry* next = entry->next;
            free(entry->key);
            if (free_values) free(entry->value);
            free(entry);
            entry = next;
        }
    }

    free(ht->buckets);
    free(ht);
}

/* =========================================================================
 *  Operations
 * ========================================================================= */

int ht_put(HashTable* ht, const char* key, void* value)
{
    if (!ht || !key) return -1;

    uint32_t hash = ht_hash_string(key);
    int bucket = (int)(hash % (uint32_t)ht->bucket_count);

    /* Check if key already exists (update) */
    for (HTEntry* e = ht->buckets[bucket]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            e->value = value;
            return 0;
        }
    }

    /* Insert new entry at head of bucket chain */
    HTEntry* entry = calloc(1, sizeof(HTEntry));
    if (!entry) return -1;

    entry->key = strdup(key);
    if (!entry->key) {
        free(entry);
        return -1;
    }
    entry->value = value;
    entry->next  = ht->buckets[bucket];
    ht->buckets[bucket] = entry;
    ht->size++;

    return 0;
}

void* ht_get(const HashTable* ht, const char* key)
{
    if (!ht || !key) return NULL;

    uint32_t hash = ht_hash_string(key);
    int bucket = (int)(hash % (uint32_t)ht->bucket_count);

    for (HTEntry* e = ht->buckets[bucket]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) return e->value;
    }
    return NULL;
}

void* ht_remove(HashTable* ht, const char* key)
{
    if (!ht || !key) return NULL;

    uint32_t hash = ht_hash_string(key);
    int bucket = (int)(hash % (uint32_t)ht->bucket_count);

    HTEntry* prev = NULL;
    for (HTEntry* e = ht->buckets[bucket]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            void* value = e->value;

            /* Unlink */
            if (prev) prev->next = e->next;
            else      ht->buckets[bucket] = e->next;

            free(e->key);
            free(e);
            ht->size--;
            return value;
        }
        prev = e;
    }
    return NULL;
}

int ht_contains(const HashTable* ht, const char* key)
{
    return ht_get(ht, key) != NULL;
}

int ht_size(const HashTable* ht)
{
    return ht ? ht->size : 0;
}

/* =========================================================================
 *  Iteration
 * ========================================================================= */

void ht_foreach(HashTable* ht, HTIterFn fn, void* userdata)
{
    if (!ht || !fn) return;

    for (int i = 0; i < ht->bucket_count; i++) {
        for (HTEntry* e = ht->buckets[i]; e; e = e->next) {
            if (fn(e->key, e->value, userdata)) return;  /* Early stop */
        }
    }
}
