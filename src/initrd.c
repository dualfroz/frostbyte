#include "initrd.h"
#include "string.h"
#include "console.h"

typedef struct UstarHeader {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
} __attribute__((packed)) UstarHeader;

static const uint8* archiveBase;
static const uint8* archiveLimit;

static uint64 parseOctal(const char* field, uint32 length)
{
    uint64 value = 0;
    uint32 i = 0;
    while (i < length && (field[i] == ' ' || field[i] == 0))
        i++;
    while (i < length && field[i] >= '0' && field[i] <= '7') {
        value = (value << 3) | (uint64)(field[i] - '0');
        i++;
    }
    return value;
}

static bool validMagic(const UstarHeader* header)
{
    return header->magic[0] == 'u' && header->magic[1] == 's' &&
           header->magic[2] == 't' && header->magic[3] == 'a' &&
           header->magic[4] == 'r';
}

void initrdInit(uint64 start, uint64 end)
{
    archiveBase = (const uint8*)start;
    archiveLimit = (const uint8*)end;

    if (end <= start || (end - start) < 512) {
        archiveBase = nullptr;
        archiveLimit = nullptr;
        kprintf("[ustar] module too small or missing\n");
        return;
    }
    kprintf("[ustar] archive at %p-%p (%lu KiB)\n",
            (void*)start, (void*)end, (end - start) / 1024);
}

static bool walk(uint32 wanted, UstarEntry* out, uint32* total)
{
    if (!archiveBase)
        return false;

    const uint8* cursor = archiveBase;
    uint32 index = 0;

    while (cursor + 512 <= archiveLimit) {
        const UstarHeader* header = (const UstarHeader*)cursor;
        if (header->name[0] == 0)
            break;
        if (!validMagic(header))
            break;

        uint64 size = parseOctal(header->size, sizeof(header->size));

        if (out && index == wanted) {
            out->name = header->name;
            out->data = cursor + 512;
            out->size = size;
            out->isDir = header->typeflag == '5';
            return true;
        }

        uint64 advance = 512 + ((size + 511) & ~(uint64)511);
        cursor += advance;
        index++;
    }

    if (total)
        *total = index;
    return false;
}

uint32 initrdCount(void)
{
    uint32 total = 0;
    walk((uint32)-1, nullptr, &total);
    return total;
}

bool initrdAt(uint32 index, UstarEntry* out)
{
    return walk(index, out, nullptr);
}
