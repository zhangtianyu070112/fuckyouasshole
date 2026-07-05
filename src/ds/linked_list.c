/**
 * @file    linked_list.c
 * @brief   Generic doubly-linked list implementation.
 */

#include "linked_list.h"
#include <stdlib.h>
#include <string.h>

/* --- Helpers ----------------------------------------------------------- */

static LLNode* node_create(void* data)
{
    LLNode* node = calloc(1, sizeof(LLNode));
    if (node) node->data = data;
    return node;
}

static LLNode* node_at(LinkedList* list, int index)
{
    if (index < 0) index = list->size + index;  /* Negative = from end */
    if (index < 0 || index >= list->size) return NULL;

    /* Choose direction for O(n/2) */
    if (index < list->size / 2) {
        LLNode* cur = list->head;
        for (int i = 0; i < index && cur; i++) cur = cur->next;
        return cur;
    } else {
        LLNode* cur = list->tail;
        for (int i = list->size - 1; i > index && cur; i--) cur = cur->prev;
        return cur;
    }
}

/* --- Lifecycle --------------------------------------------------------- */

LinkedList* ll_create(void)
{
    return calloc(1, sizeof(LinkedList));
}

void ll_destroy(LinkedList* list, int free_data)
{
    if (!list) return;
    ll_clear(list, free_data);
    free(list);
}

/* --- Mutation ---------------------------------------------------------- */

int ll_push_back(LinkedList* list, void* data)
{
    if (!list) return -1;
    LLNode* node = node_create(data);
    if (!node) return -1;

    if (list->tail) {
        list->tail->next = node;
        node->prev = list->tail;
        list->tail = node;
    } else {
        list->head = list->tail = node;
    }
    list->size++;
    return 0;
}

int ll_push_front(LinkedList* list, void* data)
{
    if (!list) return -1;
    LLNode* node = node_create(data);
    if (!node) return -1;

    if (list->head) {
        list->head->prev = node;
        node->next = list->head;
        list->head = node;
    } else {
        list->head = list->tail = node;
    }
    list->size++;
    return 0;
}

void* ll_pop_back(LinkedList* list)
{
    if (!list || !list->tail) return NULL;
    LLNode* node = list->tail;
    void* data = node->data;

    list->tail = node->prev;
    if (list->tail) list->tail->next = NULL;
    else            list->head = NULL;

    free(node);
    list->size--;
    return data;
}

void* ll_pop_front(LinkedList* list)
{
    if (!list || !list->head) return NULL;
    LLNode* node = list->head;
    void* data = node->data;

    list->head = node->next;
    if (list->head) list->head->prev = NULL;
    else            list->tail = NULL;

    free(node);
    list->size--;
    return data;
}

int ll_insert_at(LinkedList* list, int index, void* data)
{
    if (!list) return -1;
    if (index < 0) index = list->size + index;
    if (index < 0) index = 0;
    if (index >= list->size) return ll_push_back(list, data);
    if (index == 0) return ll_push_front(list, data);

    LLNode* at = node_at(list, index);
    if (!at) return -1;

    LLNode* node = node_create(data);
    if (!node) return -1;

    /* Insert before `at` */
    node->prev = at->prev;
    node->next = at;
    if (at->prev) at->prev->next = node;
    at->prev = node;
    if (at == list->head) list->head = node;

    list->size++;
    return 0;
}

void* ll_remove_at(LinkedList* list, int index)
{
    if (!list) return NULL;
    LLNode* node = node_at(list, index);
    if (!node) return NULL;

    void* data = node->data;

    if (node->prev) node->prev->next = node->next;
    else            list->head = node->next;
    if (node->next) node->next->prev = node->prev;
    else            list->tail = node->prev;

    free(node);
    list->size--;
    return data;
}

int ll_remove_data(LinkedList* list, void* target)
{
    if (!list) return 0;
    for (LLNode* cur = list->head; cur; cur = cur->next) {
        if (cur->data == target) {
            if (cur->prev) cur->prev->next = cur->next;
            else           list->head = cur->next;
            if (cur->next) cur->next->prev = cur->prev;
            else           list->tail = cur->prev;
            free(cur);
            list->size--;
            return 1;
        }
    }
    return 0;
}

void ll_clear(LinkedList* list, int free_data)
{
    if (!list) return;
    LLNode* cur = list->head;
    while (cur) {
        LLNode* next = cur->next;
        if (free_data) free(cur->data);
        free(cur);
        cur = next;
    }
    list->head = NULL;
    list->tail = NULL;
    list->size = 0;
}

/* --- Access ------------------------------------------------------------ */

void* ll_get(const LinkedList* list, int index)
{
    if (!list) return NULL;
    /* node_at takes non-const but we cast for internal use */
    LLNode* node = node_at((LinkedList*)list, index);
    return node ? node->data : NULL;
}

void* ll_first(const LinkedList* list)
{
    return list && list->head ? list->head->data : NULL;
}

void* ll_last(const LinkedList* list)
{
    return list && list->tail ? list->tail->data : NULL;
}

int ll_size(const LinkedList* list)
{
    return list ? list->size : 0;
}

int ll_is_empty(const LinkedList* list)
{
    return (!list || list->size == 0);
}

/* --- Iteration --------------------------------------------------------- */

void ll_foreach(LinkedList* list, void (*fn)(void* data, void* userdata), void* userdata)
{
    if (!list || !fn) return;
    for (LLNode* cur = list->head; cur; cur = cur->next) {
        fn(cur->data, userdata);
    }
}

void* ll_find(LinkedList* list, int (*pred)(void* data, void* userdata), void* userdata)
{
    if (!list || !pred) return NULL;
    for (LLNode* cur = list->head; cur; cur = cur->next) {
        if (pred(cur->data, userdata)) return cur->data;
    }
    return NULL;
}
