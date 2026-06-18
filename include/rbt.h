#ifndef FROSTBYTE_RBT_H
#define FROSTBYTE_RBT_H

#include "types.h"

/*
 * Rbt: an intrusive red-black tree, Linux-kernel style. Embed an RbtNode in
 * your own struct (next to a key field) and use rbtEntry() (mirrors
 * listEntry from list.h) to recover the containing struct. No allocation,
 * no element copying -- the tree only ever relinks the nodes you give it.
 *
 * Ordering is supplied by the caller as plain function pointers rather than
 * baked into the node, so the same node type can be indexed by different
 * keys in different trees (compare two full nodes for rbtInsert; compare a
 * raw key against a node for rbtFind, same convention as bsearch).
 */

typedef struct RbtNode {
    struct RbtNode* left;
    struct RbtNode* right;
    struct RbtNode* parent;
    bool red;
} RbtNode;

#define rbtEntry(ptr, type, member) \
    ((type*)((uint8*)(ptr) - __builtin_offsetof(type, member)))

/* Returns <0, 0, >0 as a compares below/equal/above b. Equal means "already
   present" -- rbtInsert rejects the new node in that case (no duplicates;
   encode a tiebreaker into the comparison if you need a multiset). */
typedef int (*RbtCompareFn)(const RbtNode* a, const RbtNode* b);

/* Same convention as bsearch: <0 if key sorts before node, >0 if after. */
typedef int (*RbtLookupFn)(const void* key, const RbtNode* node);

typedef struct RbTree {
    RbtNode* root;
    usize count;
} RbTree;

void rbtInit(RbTree* t);

/* Links node into the tree according to compare. Returns false (node left
   untouched) if a node comparing equal is already present. */
bool rbtInsert(RbTree* t, RbtNode* node, RbtCompareFn compare);

/* Unlinks node from the tree. node must currently be a member of t. */
void rbtRemove(RbTree* t, RbtNode* node);

RbtNode* rbtFind(const RbTree* t, const void* key, RbtLookupFn lookup);

RbtNode* rbtFirst(const RbTree* t);   /* leftmost (minimum) */
RbtNode* rbtLast(const RbTree* t);    /* rightmost (maximum) */

/* In-order successor/predecessor of node. nullptr past either end. Walking
   rbtFirst()..rbtNext()..nullptr visits every node in ascending order. */
RbtNode* rbtNext(const RbtNode* node);
RbtNode* rbtPrev(const RbtNode* node);

usize rbtCount(const RbTree* t);
bool rbtIsEmpty(const RbTree* t);

#endif
