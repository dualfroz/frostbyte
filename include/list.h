#ifndef FROSTBYTE_LIST_H
#define FROSTBYTE_LIST_H

#include "types.h"
#include <stddef.h>

/*
 * List: an intrusive circular doubly-linked list, Linux-kernel style. The
 * list head and every node share the same ListNode type; a sentinel node
 * (passed to listInit) acts as both head and tail so insert/remove never
 * need a null check. Embed a ListNode in your own struct and use
 * listEntry() to recover the containing struct pointer -- no allocation,
 * no element copying, just pointer relinking.
 */

typedef struct ListNode {
    struct ListNode* next;
    struct ListNode* prev;
} ListNode;

#define listEntry(ptr, type, member) \
    ((type*)((uint8*)(ptr) - offsetof(type, member)))

#define listForEach(cursor, head) \
    for ((cursor) = (head)->next; (cursor) != (head); (cursor) = (cursor)->next)

void listInit(ListNode* head);
bool listEmpty(const ListNode* head);

void listInsertAfter(ListNode* pos, ListNode* newNode);
void listInsertBefore(ListNode* pos, ListNode* newNode);
void listPushBack(ListNode* head, ListNode* newNode);
void listPushFront(ListNode* head, ListNode* newNode);

/* Unlinks node from whatever list it is in. Safe to call again on an
   already-removed node (it becomes its own trivial one-element list). */
void listRemove(ListNode* node);

usize listLen(const ListNode* head);
ListNode* listFront(const ListNode* head);
ListNode* listBack(const ListNode* head);

#endif
