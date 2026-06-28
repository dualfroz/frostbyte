#include "list.h"

void listInit(ListNode* head)
{
    head->next = head;
    head->prev = head;
}

bool listEmpty(const ListNode* head)
{
    return head->next == head;
}

void listInsertAfter(ListNode* pos, ListNode* newNode)
{
    newNode->prev = pos;
    newNode->next = pos->next;
    pos->next->prev = newNode;
    pos->next = newNode;
}

void listInsertBefore(ListNode* pos, ListNode* newNode)
{
    listInsertAfter(pos->prev, newNode);
}

void listPushBack(ListNode* head, ListNode* newNode)
{
    listInsertBefore(head, newNode);
}

void listPushFront(ListNode* head, ListNode* newNode)
{
    listInsertAfter(head, newNode);
}

void listRemove(ListNode* node)
{
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->next = node;
    node->prev = node;
}

usize listLen(const ListNode* head)
{
    usize count = 0;
    for (const ListNode* cursor = head->next; cursor != head; cursor = cursor->next)
        count++;
    return count;
}

ListNode* listFront(const ListNode* head)
{
    return listEmpty(head) ? nullptr : head->next;
}

ListNode* listBack(const ListNode* head)
{
    return listEmpty(head) ? nullptr : head->prev;
}
