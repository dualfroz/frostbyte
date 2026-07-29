#include "ktest.h"
#include "rbt.h"
#include "heap.h"

typedef struct IntNode {
    RbtNode node;
    int32 key;
} IntNode;

static int compareIntNode(const RbtNode* a, const RbtNode* b)
{
    const IntNode* ia = rbtEntry(a, IntNode, node);
    const IntNode* ib = rbtEntry(b, IntNode, node);
    if (ia->key < ib->key)
        return -1;
    if (ia->key > ib->key)
        return 1;
    return 0;
}

static int lookupIntKey(const void* key, const RbtNode* node)
{
    int32 k = *(const int32*)key;
    const IntNode* in = rbtEntry(node, IntNode, node);
    if (k < in->key)
        return -1;
    if (k > in->key)
        return 1;
    return 0;
}

/* Fisher-Yates over a small LCG so insertion/removal order is scrambled but
   reproducible across runs -- the point is to exercise every rotation case,
   not to be a real source of randomness. */
static void shuffle(int32* arr, int32 n, uint32 seed)
{
    uint32 state = seed;
    for (int32 i = n - 1; i > 0; i--) {
        state = state * 1664525u + 1013904223u;
        int32 j = (int32)(state % (uint32)(i + 1));
        int32 tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

/* Returns the black-height of node (counting nullptr leaves as height 1) and
   clears *valid if either red-black rule is broken anywhere in the subtree:
   a red node with a red child, or unequal black-height between the two
   sides of some node. */
static int32 checkSubtree(RbtNode* node, bool* valid)
{
    if (node == nullptr)
        return 1;

    if (node->red) {
        if ((node->left != nullptr && node->left->red) ||
            (node->right != nullptr && node->right->red))
            *valid = false;
    }
    if (node->left != nullptr && node->left->parent != node)
        *valid = false;
    if (node->right != nullptr && node->right->parent != node)
        *valid = false;

    int32 leftHeight = checkSubtree(node->left, valid);
    int32 rightHeight = checkSubtree(node->right, valid);
    if (leftHeight != rightHeight)
        *valid = false;

    return leftHeight + (node->red ? 0 : 1);
}

static bool checkInvariants(RbTree* tree)
{
    if (tree->root != nullptr && tree->root->red)
        return false;
    bool valid = true;
    checkSubtree(tree->root, &valid);
    return valid;
}

KTEST(rbtEmptyTree)
{
    RbTree tree;
    rbtInit(&tree);

    KT_TRUE(t, rbtIsEmpty(&tree));
    KT_EQ(t, rbtCount(&tree), 0);
    KT_NULL(t, rbtFirst(&tree));
    KT_NULL(t, rbtLast(&tree));

    int32 key = 42;
    KT_NULL(t, rbtFind(&tree, &key, lookupIntKey));
}

KTEST(rbtBasicInsertFindOrder)
{
    RbTree tree;
    rbtInit(&tree);

    int32 values[] = { 5, 3, 8, 1, 4, 7, 9 };
    usize n = sizeof(values) / sizeof(values[0]);
    IntNode nodes[7];

    for (usize i = 0; i < n; i++) {
        nodes[i].key = values[i];
        KT_TRUE(t, rbtInsert(&tree, &nodes[i].node, compareIntNode));
    }
    KT_EQ(t, rbtCount(&tree), n);
    KT_TRUE(t, checkInvariants(&tree));

    int32 expected[] = { 1, 3, 4, 5, 7, 8, 9 };
    usize i = 0;
    for (RbtNode* c = rbtFirst(&tree); c != nullptr; c = rbtNext(c), i++)
        KT_EQ(t, rbtEntry(c, IntNode, node)->key, expected[i]);
    KT_EQ(t, i, n);

    /* walking backwards from the max visits the same keys in reverse */
    i = n;
    for (RbtNode* c = rbtLast(&tree); c != nullptr; c = rbtPrev(c))
        KT_EQ(t, rbtEntry(c, IntNode, node)->key, expected[--i]);
    KT_EQ(t, i, 0);

    int32 hitKey = 8;
    RbtNode* found = rbtFind(&tree, &hitKey, lookupIntKey);
    KT_NOTNULL(t, found);
    if (found)
        KT_EQ(t, rbtEntry(found, IntNode, node)->key, 8);

    int32 missKey = 100;
    KT_NULL(t, rbtFind(&tree, &missKey, lookupIntKey));

    KT_EQ(t, rbtEntry(rbtFirst(&tree), IntNode, node)->key, 1);
    KT_EQ(t, rbtEntry(rbtLast(&tree), IntNode, node)->key, 9);
}

KTEST(rbtRejectsDuplicateInsert)
{
    RbTree tree;
    rbtInit(&tree);

    IntNode a, b;
    a.key = 5;
    b.key = 5;

    KT_TRUE(t, rbtInsert(&tree, &a.node, compareIntNode));
    KT_FALSE(t, rbtInsert(&tree, &b.node, compareIntNode));
    KT_EQ(t, rbtCount(&tree), 1);
}

KTEST(rbtSingleRemoveRelinksNeighbors)
{
    RbTree tree;
    rbtInit(&tree);

