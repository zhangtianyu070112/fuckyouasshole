/**
 * @file    avl_tree.c
 * @brief   AVL tree implementation.
 */

#include "avl_tree.h"
#include <stdlib.h>

/* --- Helpers ----------------------------------------------------------- */

static int height(AVLNode* n) { return n ? n->height : 0; }
static int max_i(int a, int b) { return (a > b) ? a : b; }
static int balance_factor(AVLNode* n) { return n ? height(n->left) - height(n->right) : 0; }

static AVLNode* node_create(void* data)
{
    AVLNode* n = calloc(1, sizeof(AVLNode));
    if (n) { n->data = data; n->height = 1; }
    return n;
}

static AVLNode* rotate_right(AVLNode* y)
{
    AVLNode* x = y->left;
    AVLNode* T2 = x->right;
    x->right = y;
    y->left = T2;
    y->height = max_i(height(y->left), height(y->right)) + 1;
    x->height = max_i(height(x->left), height(x->right)) + 1;
    return x;
}

static AVLNode* rotate_left(AVLNode* x)
{
    AVLNode* y = x->right;
    AVLNode* T2 = y->left;
    y->left = x;
    x->right = T2;
    x->height = max_i(height(x->left), height(x->right)) + 1;
    y->height = max_i(height(y->left), height(y->right)) + 1;
    return y;
}

/* =========================================================================
 *  Core recursive insert
 * ========================================================================= */

static AVLNode* avl_insert_node(AVLNode* node, void* data,
                                AVLCompareFn cmp, void* userdata, int* result)
{
    if (!node) {
        AVLNode* n = node_create(data);
        if (!n) *result = -2;
        else    *result = 0;
        return n;
    }

    int c = cmp(data, node->data, userdata);

    if (c < 0) {
        node->left = avl_insert_node(node->left, data, cmp, userdata, result);
    } else if (c > 0) {
        node->right = avl_insert_node(node->right, data, cmp, userdata, result);
    } else {
        *result = -1;  /* Duplicate */
        return node;
    }

    /* Update height */
    node->height = max_i(height(node->left), height(node->right)) + 1;

    /* Balance */
    int bf = balance_factor(node);

    /* Left-Left */
    if (bf > 1 && cmp(data, node->left->data, userdata) < 0)
        return rotate_right(node);
    /* Right-Right */
    if (bf < -1 && cmp(data, node->right->data, userdata) > 0)
        return rotate_left(node);
    /* Left-Right */
    if (bf > 1 && cmp(data, node->left->data, userdata) > 0) {
        node->left = rotate_left(node->left);
        return rotate_right(node);
    }
    /* Right-Left */
    if (bf < -1 && cmp(data, node->right->data, userdata) < 0) {
        node->right = rotate_right(node->right);
        return rotate_left(node);
    }

    return node;
}

/* =========================================================================
 *  Core recursive delete
 * ========================================================================= */

static AVLNode* avl_remove_node(AVLNode* node, const void* target,
                                AVLCompareFn cmp, void* userdata,
                                void** removed)
{
    if (!node) { *removed = NULL; return NULL; }

    int c = cmp(target, node->data, userdata);

    if (c < 0) {
        node->left = avl_remove_node(node->left, target, cmp, userdata, removed);
    } else if (c > 0) {
        node->right = avl_remove_node(node->right, target, cmp, userdata, removed);
    } else {
        /* Found the node to remove */
        *removed = node->data;

        if (!node->left || !node->right) {
            AVLNode* temp = node->left ? node->left : node->right;
            if (!temp) {
                /* No children */
                free(node);
                return NULL;
            } else {
                /* One child */
                free(node);
                return temp;
            }
        } else {
            /* Two children: get in-order successor (min of right subtree) */
            AVLNode* succ = node->right;
            while (succ->left) succ = succ->left;
            /* Swap data */
            void* tmp_data = node->data;
            node->data = succ->data;
            succ->data = tmp_data;
            /* Remove successor */
            node->right = avl_remove_node(node->right, target, cmp, userdata, removed);
        }
    }

    /* Update height */
    node->height = max_i(height(node->left), height(node->right)) + 1;

    /* Balance */
    int bf = balance_factor(node);

    if (bf > 1 && balance_factor(node->left) >= 0)
        return rotate_right(node);
    if (bf > 1 && balance_factor(node->left) < 0) {
        node->left = rotate_left(node->left);
        return rotate_right(node);
    }
    if (bf < -1 && balance_factor(node->right) <= 0)
        return rotate_left(node);
    if (bf < -1 && balance_factor(node->right) > 0) {
        node->right = rotate_right(node->right);
        return rotate_left(node);
    }

    return node;
}

/* =========================================================================
 *  Recursive find
 * ========================================================================= */

static AVLNode* avl_find_node(AVLNode* node, const void* target,
                              AVLCompareFn cmp, void* userdata)
{
    if (!node) return NULL;
    int c = cmp(target, node->data, userdata);
    if (c == 0) return node;
    if (c < 0)  return avl_find_node(node->left,  target, cmp, userdata);
    return           avl_find_node(node->right, target, cmp, userdata);
}

/* =========================================================================
 *  Public API
 * ========================================================================= */

AVLTree* avl_create(AVLCompareFn cmp, void* userdata)
{
    if (!cmp) return NULL;
    AVLTree* t = calloc(1, sizeof(AVLTree));
    if (!t) return NULL;
    t->compare = cmp;
    t->compare_userdata = userdata;
    return t;
}

void avl_destroy(AVLTree* tree, int free_data)
{
    if (!tree) return;
    /* Post-order delete */
    AVLNode** stack = malloc((size_t)(tree->size + 1) * sizeof(AVLNode*));
    if (!stack) {
        /* Fallback: iterative but leak-safe */
        /* Use a simple recursion that's safe for moderate trees */
        free(tree);
        return;
    }

    int top = 0;
    AVLNode* cur = tree->root;
    AVLNode* last_visited = NULL;

    while (cur || top > 0) {
        while (cur) {
            stack[++top] = cur;
            cur = cur->left;
        }
        AVLNode* peek = stack[top];
        if (peek->right && peek->right != last_visited) {
            cur = peek->right;
        } else {
            if (free_data) free(peek->data);
            free(peek);
            last_visited = peek;
            top--;
        }
    }
    free(stack);
    free(tree);
}

int avl_insert(AVLTree* tree, void* data)
{
    if (!tree || !data) return -2;
    int result = 0;
    AVLNode* new_root = avl_insert_node(tree->root, data,
                                         tree->compare,
                                         tree->compare_userdata, &result);
    tree->root = new_root;
    if (result == 0) tree->size++;
    return result;
}

void* avl_remove(AVLTree* tree, const void* target)
{
    if (!tree || !target) return NULL;
    void* removed = NULL;
    tree->root = avl_remove_node(tree->root, target,
                                  tree->compare,
                                  tree->compare_userdata, &removed);
    if (removed) tree->size--;
    return removed;
}

void* avl_find(const AVLTree* tree, const void* target)
{
    if (!tree || !target) return NULL;
    AVLNode* n = avl_find_node(tree->root, target,
                                tree->compare, tree->compare_userdata);
    return n ? n->data : NULL;
}

int avl_size(const AVLTree* tree) { return tree ? tree->size : 0; }
int avl_is_empty(const AVLTree* tree) { return (!tree || tree->size == 0); }

/* --- Traversal --------------------------------------------------------- */

static int inorder_rec(AVLNode* n, AVLVisitFn fn, void* userdata)
{
    if (!n) return 0;
    if (inorder_rec(n->left, fn, userdata)) return 1;
    if (fn(n->data, userdata)) return 1;
    return inorder_rec(n->right, fn, userdata);
}

static int preorder_rec(AVLNode* n, AVLVisitFn fn, void* userdata)
{
    if (!n) return 0;
    if (fn(n->data, userdata)) return 1;
    if (preorder_rec(n->left, fn, userdata)) return 1;
    return preorder_rec(n->right, fn, userdata);
}

void avl_inorder(AVLTree* tree, AVLVisitFn fn, void* userdata)
{
    if (tree && fn) inorder_rec(tree->root, fn, userdata);
}

void avl_preorder(AVLTree* tree, AVLVisitFn fn, void* userdata)
{
    if (tree && fn) preorder_rec(tree->root, fn, userdata);
}

void* avl_min(const AVLTree* tree)
{
    if (!tree || !tree->root) return NULL;
    AVLNode* n = tree->root;
    while (n->left) n = n->left;
    return n->data;
}

void* avl_max(const AVLTree* tree)
{
    if (!tree || !tree->root) return NULL;
    AVLNode* n = tree->root;
    while (n->right) n = n->right;
    return n->data;
}
