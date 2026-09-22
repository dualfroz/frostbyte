#ifndef FROSTBYTE_LIST_H
#define FROSTBYTE_LIST_H

#include "types.h"
#include <stddef.h>

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

void listRemove(ListNode* node);

usize listLen(const ListNode* head);
ListNode* listFront(const ListNode* head);
ListNode* listBack(const ListNode* head);

#endif
