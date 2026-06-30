#include "rbt.h"

static bool isRed(const RbtNode* n)
{
    return n != nullptr && n->red;
}

static void setChild(RbTree* t, RbtNode* parent, RbtNode* oldChild, RbtNode* newChild)
{
    if (parent == nullptr)
        t->root = newChild;
    else if (parent->left == oldChild)
        parent->left = newChild;
    else
        parent->right = newChild;
}

static void rotateLeft(RbTree* t, RbtNode* x)
{
    RbtNode* y = x->right;
    x->right = y->left;
    if (y->left != nullptr)
        y->left->parent = x;
    y->parent = x->parent;
    setChild(t, x->parent, x, y);
    y->left = x;
    x->parent = y;
}

static void rotateRight(RbTree* t, RbtNode* x)
{
    RbtNode* y = x->left;
    x->left = y->right;
    if (y->right != nullptr)
        y->right->parent = x;
    y->parent = x->parent;
    setChild(t, x->parent, x, y);
    y->right = x;
    x->parent = y;
}

static void insertFixup(RbTree* t, RbtNode* z)
{
    while (isRed(z->parent)) {
        RbtNode* parent = z->parent;
        RbtNode* grandparent = parent->parent;

        if (parent == grandparent->left) {
            RbtNode* uncle = grandparent->right;
            if (isRed(uncle)) {
                parent->red = false;
                uncle->red = false;
                grandparent->red = true;
                z = grandparent;
            } else {
                if (z == parent->right) {
                    z = parent;
                    rotateLeft(t, z);
                    parent = z->parent;
                }
                parent->red = false;
                grandparent->red = true;
                rotateRight(t, grandparent);
            }
        } else {
            RbtNode* uncle = grandparent->left;
            if (isRed(uncle)) {
                parent->red = false;
                uncle->red = false;
                grandparent->red = true;
                z = grandparent;
            } else {
                if (z == parent->left) {
                    z = parent;
                    rotateRight(t, z);
                    parent = z->parent;
                }
                parent->red = false;
                grandparent->red = true;
                rotateLeft(t, grandparent);
            }
        }
    }
    t->root->red = false;
}

static void removeFixup(RbTree* t, RbtNode* x, RbtNode* xParent)
{
    while (x != t->root && !isRed(x)) {
        if (x == xParent->left) {
            RbtNode* w = xParent->right;
            if (isRed(w)) {
                w->red = false;
                xParent->red = true;
                rotateLeft(t, xParent);
                w = xParent->right;
            }
            if (!isRed(w->left) && !isRed(w->right)) {
                w->red = true;
                x = xParent;
                xParent = x->parent;
            } else {
                if (!isRed(w->right)) {
                    if (w->left != nullptr)
                        w->left->red = false;
                    w->red = true;
                    rotateRight(t, w);
                    w = xParent->right;
                }
                w->red = xParent->red;
                xParent->red = false;
                if (w->right != nullptr)
                    w->right->red = false;
                rotateLeft(t, xParent);
                x = t->root;
                xParent = nullptr;
            }
        } else {
            RbtNode* w = xParent->left;
            if (isRed(w)) {
                w->red = false;
                xParent->red = true;
                rotateRight(t, xParent);
                w = xParent->left;
            }
            if (!isRed(w->left) && !isRed(w->right)) {
                w->red = true;
                x = xParent;
                xParent = x->parent;
            } else {
                if (!isRed(w->left)) {
                    if (w->right != nullptr)
                        w->right->red = false;
                    w->red = true;
                    rotateLeft(t, w);
                    w = xParent->left;
                }
                w->red = xParent->red;
                xParent->red = false;
                if (w->left != nullptr)
                    w->left->red = false;
                rotateRight(t, xParent);
                x = t->root;
                xParent = nullptr;
            }
        }
    }
    if (x != nullptr)
        x->red = false;
}

void rbtInit(RbTree* t)
{
    t->root = nullptr;
    t->count = 0;
}

bool rbtInsert(RbTree* t, RbtNode* node, RbtCompareFn compare)
{
    RbtNode* parent = nullptr;
    RbtNode* cur = t->root;
    bool goLeft = false;

    while (cur != nullptr) {
        int c = compare(node, cur);
        if (c == 0)
            return false;
        parent = cur;
        goLeft = c < 0;
        cur = goLeft ? cur->left : cur->right;
    }

    node->left = nullptr;
    node->right = nullptr;
    node->parent = parent;
    node->red = true;

    if (parent == nullptr)
        t->root = node;
    else if (goLeft)
        parent->left = node;
    else
        parent->right = node;

    t->count++;
    insertFixup(t, node);
    return true;
}

void rbtRemove(RbTree* t, RbtNode* z)
{
    RbtNode* y = z;
    bool yOriginalRed = y->red;
    RbtNode* x;
    RbtNode* xParent;

    if (z->left == nullptr) {
        x = z->right;
        xParent = z->parent;
        setChild(t, z->parent, z, z->right);
        if (z->right != nullptr)
            z->right->parent = z->parent;
    } else if (z->right == nullptr) {
        x = z->left;
        xParent = z->parent;
        setChild(t, z->parent, z, z->left);
        if (z->left != nullptr)
            z->left->parent = z->parent;
    } else {
        y = z->right;
        while (y->left != nullptr)
            y = y->left;
        yOriginalRed = y->red;
        x = y->right;

        if (y->parent == z) {
            xParent = y;
        } else {
            xParent = y->parent;
            setChild(t, y->parent, y, y->right);
            if (y->right != nullptr)
                y->right->parent = y->parent;
            y->right = z->right;
            y->right->parent = y;
        }

        setChild(t, z->parent, z, y);
        y->parent = z->parent;
        y->left = z->left;
        y->left->parent = y;
        y->red = z->red;
    }

    t->count--;
    if (!yOriginalRed)
        removeFixup(t, x, xParent);
}

RbtNode* rbtFind(const RbTree* t, const void* key, RbtLookupFn lookup)
{
    RbtNode* cur = t->root;
    while (cur != nullptr) {
        int c = lookup(key, cur);
        if (c == 0)
            return cur;
        cur = c < 0 ? cur->left : cur->right;
    }
    return nullptr;
}

RbtNode* rbtFirst(const RbTree* t)
{
    RbtNode* n = t->root;
    if (n == nullptr)
        return nullptr;
    while (n->left != nullptr)
        n = n->left;
    return n;
}

RbtNode* rbtLast(const RbTree* t)
{
    RbtNode* n = t->root;
    if (n == nullptr)
        return nullptr;
    while (n->right != nullptr)
        n = n->right;
    return n;
}

RbtNode* rbtNext(const RbtNode* node)
{
    if (node->right != nullptr) {
        RbtNode* n = node->right;
        while (n->left != nullptr)
            n = n->left;
        return n;
    }
    RbtNode* n = (RbtNode*)node;
    RbtNode* parent = n->parent;
    while (parent != nullptr && n == parent->right) {
        n = parent;
        parent = parent->parent;
    }
    return parent;
}

RbtNode* rbtPrev(const RbtNode* node)
{
    if (node->left != nullptr) {
        RbtNode* n = node->left;
        while (n->right != nullptr)
            n = n->right;
        return n;
    }
    RbtNode* n = (RbtNode*)node;
    RbtNode* parent = n->parent;
    while (parent != nullptr && n == parent->left) {
        n = parent;
        parent = parent->parent;
    }
    return parent;
}

usize rbtCount(const RbTree* t)
{
    return t->count;
}

bool rbtIsEmpty(const RbTree* t)
{
    return t->root == nullptr;
}
