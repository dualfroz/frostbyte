#ifndef FROSTBYTE_RBT_H
#define FROSTBYTE_RBT_H

#include "types.h"

typedef struct RbtNode {
    struct RbtNode* left;
    struct RbtNode* right;
    struct RbtNode* parent;
    bool red;
} RbtNode;

#define rbtEntry(ptr, type, member) \
    ((type*)((uint8*)(ptr) - __builtin_offsetof(type, member)))

typedef int (*RbtCompareFn)(const RbtNode* a, const RbtNode* b);

typedef int (*RbtLookupFn)(const void* key, const RbtNode* node);

typedef struct RbTree {
    RbtNode* root;
    usize count;
} RbTree;

void rbtInit(RbTree* t);

bool rbtInsert(RbTree* t, RbtNode* node, RbtCompareFn compare);

void rbtRemove(RbTree* t, RbtNode* node);

RbtNode* rbtFind(const RbTree* t, const void* key, RbtLookupFn lookup);

RbtNode* rbtFirst(const RbTree* t);
RbtNode* rbtLast(const RbTree* t);

RbtNode* rbtNext(const RbtNode* node);
RbtNode* rbtPrev(const RbtNode* node);

usize rbtCount(const RbTree* t);
bool rbtIsEmpty(const RbTree* t);

#endif
