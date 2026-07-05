/**
 * @file    avl_tree.h
 * @brief   Generic AVL (Adelson-Velsky & Landis) self-balancing binary search tree.
 *
 * Stores void* values with a user-provided comparator function.
 * Guarantees O(log n) insert, delete, and lookup.
 *
 * Use case: maintaining sorted waypoint lists, nearest-neighbor queries,
 *           autocomplete suggestions for FMC airport entry.
 */

#ifndef AVL_TREE_H
#define AVL_TREE_H

#include <stdint.h>

/* --- Types ------------------------------------------------------------- */

/**
 * @brief Comparator function.
 * @return < 0 if a < b, 0 if a == b, > 0 if a > b.
 */
typedef int (*AVLCompareFn)(const void* a, const void* b, void* userdata);

typedef struct AVLNode {
    void*           data;
    struct AVLNode* left;
    struct AVLNode* right;
    int             height;
} AVLNode;

typedef struct {
    AVLNode*     root;
    AVLCompareFn compare;
    void*        compare_userdata;  /* Passed as 3rd arg to compare */
    int          size;
} AVLTree;

/* --- Lifecycle --------------------------------------------------------- */

/**
 * @brief Create an AVL tree.
 * @param cmp       Comparator function (required).
 * @param userdata  Opaque userdata passed to comparator.
 */
AVLTree* avl_create(AVLCompareFn cmp, void* userdata);

/**
 * @brief Destroy the tree. If free_data != 0, calls free() on each data pointer.
 */
void avl_destroy(AVLTree* tree, int free_data);

/* --- Operations -------------------------------------------------------- */

/** Insert data. Returns 0 on success, -1 on duplicate, -2 on allocation failure. */
int avl_insert(AVLTree* tree, void* data);

/** Remove and return data equal to target (by comparator returning 0). */
void* avl_remove(AVLTree* tree, const void* target);

/** Find data equal to target, or NULL. */
void* avl_find(const AVLTree* tree, const void* target);

/** Return number of nodes. */
int avl_size(const AVLTree* tree);

/** Return 1 if empty. */
int avl_is_empty(const AVLTree* tree);

/* --- Traversal --------------------------------------------------------- */

/** In-order traversal callback. Return non-zero to stop. */
typedef int (*AVLVisitFn)(void* data, void* userdata);

/** In-order traversal (sorted order). */
void avl_inorder(AVLTree* tree, AVLVisitFn fn, void* userdata);

/** Pre-order traversal. */
void avl_preorder(AVLTree* tree, AVLVisitFn fn, void* userdata);

/** Find the minimum (leftmost) element. */
void* avl_min(const AVLTree* tree);

/** Find the maximum (rightmost) element. */
void* avl_max(const AVLTree* tree);

#endif /* AVL_TREE_H */
