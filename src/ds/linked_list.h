/**
 * @file    linked_list.h
 * @brief   Generic doubly-linked list.
 *
 * Stores void* values. Supports insert/remove at any position, iteration,
 * and standard queue/stack operations. The caller owns the stored data;
 * the list only manages the node structures.
 */

#ifndef LINKED_LIST_H
#define LINKED_LIST_H

#include <stdint.h>

/* --- Node & List types ------------------------------------------------- */

typedef struct LLNode {
    void*           data;
    struct LLNode*  prev;
    struct LLNode*  next;
} LLNode;

typedef struct {
    LLNode* head;
    LLNode* tail;
    int     size;
} LinkedList;

/* --- Lifecycle --------------------------------------------------------- */

/** Create an empty linked list. */
LinkedList* ll_create(void);

/**
 * @brief Destroy the list and all its nodes.
 * @param free_data  If non-zero, calls free() on each data pointer.
 */
void ll_destroy(LinkedList* list, int free_data);

/* --- Mutation ---------------------------------------------------------- */

/** Append to end. Returns 0 on success, -1 on allocation failure. */
int ll_push_back(LinkedList* list, void* data);

/** Prepend to beginning. */
int ll_push_front(LinkedList* list, void* data);

/** Remove and return the last element's data, or NULL if empty. */
void* ll_pop_back(LinkedList* list);

/** Remove and return the first element's data, or NULL if empty. */
void* ll_pop_front(LinkedList* list);

/** Insert data at the given index (0-based). Negative = from end. */
int ll_insert_at(LinkedList* list, int index, void* data);

/** Remove and return data at the given index, or NULL. */
void* ll_remove_at(LinkedList* list, int index);

/** Remove the first node whose data == target (pointer equality). Returns 1 if removed. */
int ll_remove_data(LinkedList* list, void* target);

/** Remove all nodes. If free_data != 0, calls free() on each data pointer. */
void ll_clear(LinkedList* list, int free_data);

/* --- Access ------------------------------------------------------------ */

/** Get data at index (0-based). Negative = from end. Returns NULL if out of bounds. */
void* ll_get(const LinkedList* list, int index);

/** Get first element. */
void* ll_first(const LinkedList* list);

/** Get last element. */
void* ll_last(const LinkedList* list);

/** Return size. */
int ll_size(const LinkedList* list);

/** Return 1 if empty. */
int ll_is_empty(const LinkedList* list);

/* --- Iteration --------------------------------------------------------- */

/** Apply function to each element. fn(data, userdata) */
void ll_foreach(LinkedList* list, void (*fn)(void* data, void* userdata), void* userdata);

/** Find first element where predicate returns non-zero. */
void* ll_find(LinkedList* list, int (*pred)(void* data, void* userdata), void* userdata);

#endif /* LINKED_LIST_H */
