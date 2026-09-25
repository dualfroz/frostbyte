#include "ktest.h"
#include "list.h"

typedef struct TestItem {
    int value;
    ListNode node;
} TestItem;

KTEST(listEmptyInit)
{
    ListNode head;
    listInit(&head);
    KT_TRUE(t, listEmpty(&head));
    KT_EQ(t, listLen(&head), 0);
    KT_NULL(t, listFront(&head));
    KT_NULL(t, listBack(&head));
}

KTEST(listPushBackOrder)
{
    ListNode head;
    listInit(&head);

    TestItem items[4];
    for (int i = 0; i < 4; i++) {
        items[i].value = i;
        listPushBack(&head, &items[i].node);
    }

    KT_FALSE(t, listEmpty(&head));
    KT_EQ(t, listLen(&head), 4);

    int expected = 0;
    ListNode* cursor;
    listForEach(cursor, &head) {
        TestItem* item = listEntry(cursor, TestItem, node);
        KT_EQ(t, item->value, expected);
        expected++;
    }
    KT_EQ(t, expected, 4);

    TestItem* front = listEntry(listFront(&head), TestItem, node);
    TestItem* back = listEntry(listBack(&head), TestItem, node);
    KT_EQ(t, front->value, 0);
    KT_EQ(t, back->value, 3);
}

KTEST(listPushFrontOrder)
{
    ListNode head;
    listInit(&head);

    TestItem items[3];
    for (int i = 0; i < 3; i++) {
        items[i].value = i;
        listPushFront(&head, &items[i].node);
    }
    int expected[3] = { 2, 1, 0 };
    int idx = 0;
    ListNode* cursor;
    listForEach(cursor, &head) {
        TestItem* item = listEntry(cursor, TestItem, node);
        KT_EQ(t, item->value, expected[idx]);
        idx++;
    }
}

KTEST(listRemoveMiddle)
{
    ListNode head;
    listInit(&head);

    TestItem items[3];
    for (int i = 0; i < 3; i++) {
        items[i].value = i;
        listPushBack(&head, &items[i].node);
    }

    listRemove(&items[1].node);
    KT_EQ(t, listLen(&head), 2);

    int idx = 0;
    int expected[2] = { 0, 2 };
    ListNode* cursor;
    listForEach(cursor, &head) {
        TestItem* item = listEntry(cursor, TestItem, node);
        KT_EQ(t, item->value, expected[idx]);
        idx++;
    }

    KT_TRUE(t, listEmpty(&items[1].node));
}

KTEST(listRemoveAllEmptiesList)
{
    ListNode head;
    listInit(&head);

    TestItem items[2];
    items[0].value = 10;
    items[1].value = 20;
    listPushBack(&head, &items[0].node);
    listPushBack(&head, &items[1].node);

    listRemove(&items[0].node);
    listRemove(&items[1].node);

    KT_TRUE(t, listEmpty(&head));
    KT_EQ(t, listLen(&head), 0);
}
