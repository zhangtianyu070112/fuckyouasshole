/**
 * @file    hash_table.h
 * @brief   Generic hash table with string keys.
 *
 * Uses separate chaining (linked list per bucket) for collision resolution.
 * Supports insert, lookup, delete, and iteration over all entries.
 * The caller owns keys and values; the table makes internal copies of keys.
 *
 * Use case: fast airport/waypoint lookup by ICAO/identifier.
 */

#ifndef HASH_TABLE_H
#define HASH_TABLE_H

#include <stdint.h>

/* --- Types ------------------------------------------------------------- */

typedef struct HTEntry {
    char*            key;
    void*            value;
    struct HTEntry*  next;  /* Chaining for collision resolution */
} HTEntry;

typedef struct {
    HTEntry** buckets;
    int       bucket_count;
    int       size;          /* Number of entries */
} HashTable;

/* --- Lifecycle --------------------------------------------------------- */

/**
 * @brief Create a hash table.
 * @param bucket_count  Number of buckets (use prime numbers: 101, 503, 1009, ...).
 *                      If 0, defaults to 211.
 */
HashTable* ht_create(int bucket_count);

/**
 * @brief Destroy the table. If free_values != 0, calls free() on each value.
 */
void ht_destroy(HashTable* ht, int free_values);

/* --- Operations -------------------------------------------------------- */

/**
 * @brief Insert or update a key-value pair.
 *        Key is copied internally; value pointer is stored as-is.
 * @return 0 on success, -1 on allocation failure.
 */
int ht_put(HashTable* ht, const char* key, void* value);

/**
 * @brief Look up a key. Returns value pointer or NULL if not found.
 *        Note: a NULL value means "not found" — don't store NULL values.
 */
void* ht_get(const HashTable* ht, const char* key);

/**
 * @brief Remove a key. Returns the old value (caller may free), or NULL.
 */
void* ht_remove(HashTable* ht, const char* key);

/**
 * @brief Check if key exists.
 */
int ht_contains(const HashTable* ht, const char* key);

/**
 * @brief Number of entries.
 */
int ht_size(const HashTable* ht);

/* --- Iteration --------------------------------------------------------- */

/** Callback for ht_foreach. Return non-zero to stop iteration early. */
typedef int (*HTIterFn)(const char* key, void* value, void* userdata);

/**
 * @brief Iterate over all entries. Order is not guaranteed.
 * @param fn  Called for each entry. Return 1 to stop early.
 */
void ht_foreach(HashTable* ht, HTIterFn fn, void* userdata);

/* --- Internal ---------------------------------------------------------- */

/** DJB2 hash function for string keys. */
uint32_t ht_hash_string(const char* key);

#endif /* HASH_TABLE_H */
