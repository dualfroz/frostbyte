#include "vec.h"
#include "heap.h"
#include "string.h"

void vecInit(Vec* v, usize elemSize)
{
    v->data = nullptr;
    v->elemSize = elemSize;
    v->length = 0;
    v->capacity = 0;
}

void vecFree(Vec* v)
{
    kfree(v->data);
    v->data = nullptr;
    v->length = 0;
    v->capacity = 0;
}

bool vecReserve(Vec* v, usize newCapacity)
{
    if (newCapacity <= v->capacity)
        return true;

    usize grown = v->capacity == 0 ? 4 : v->capacity * 2;
    if (grown < newCapacity)
        grown = newCapacity;

    uint8* bigger = (uint8*)krealloc(v->data, grown * v->elemSize);
    if (!bigger)
        return false;

    v->data = bigger;
    v->capacity = grown;
    return true;
}

bool vecPush(Vec* v, const void* elem)
{
    if (!vecReserve(v, v->length + 1))
        return false;
    memcpy(v->data + v->length * v->elemSize, elem, v->elemSize);
    v->length++;
    return true;
}

bool vecPop(Vec* v, void* outElem)
{
    if (v->length == 0)
        return false;
    v->length--;
    if (outElem)
        memcpy(outElem, v->data + v->length * v->elemSize, v->elemSize);
    return true;
}

void* vecAt(Vec* v, usize index)
{
    if (index >= v->length)
        return nullptr;
    return v->data + index * v->elemSize;
}

bool vecSet(Vec* v, usize index, const void* elem)
{
    if (index >= v->length)
        return false;
    memcpy(v->data + index * v->elemSize, elem, v->elemSize);
    return true;
}

bool vecInsert(Vec* v, usize index, const void* elem)
{
    if (index > v->length)
        return false;
    if (!vecReserve(v, v->length + 1))
        return false;

    usize tailCount = v->length - index;
    if (tailCount > 0) {
        memmove(v->data + (index + 1) * v->elemSize,
                v->data + index * v->elemSize,
                tailCount * v->elemSize);
    }
    memcpy(v->data + index * v->elemSize, elem, v->elemSize);
    v->length++;
    return true;
}

bool vecRemove(Vec* v, usize index, void* outElem)
{
    if (index >= v->length)
        return false;

    if (outElem)
        memcpy(outElem, v->data + index * v->elemSize, v->elemSize);

    usize tailCount = v->length - index - 1;
    if (tailCount > 0) {
        memmove(v->data + index * v->elemSize,
                v->data + (index + 1) * v->elemSize,
                tailCount * v->elemSize);
    }
    v->length--;
    return true;
}

void vecClear(Vec* v)
{
    v->length = 0;
}

usize vecLen(const Vec* v) { return v->length; }
usize vecCap(const Vec* v) { return v->capacity; }
bool vecIsEmpty(const Vec* v) { return v->length == 0; }
