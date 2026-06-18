#include "ulib.h"

#define blockBytes 8192

int main(void)
{
    print("sbrktest: pid=");
    printDec(userGetpid());
    print(" requesting 8192 bytes via sbrk\n");

    char* block = (char*)userSbrk(blockBytes);

    for (int i = 0; i < blockBytes; i++)
        block[i] = (char)(i & 0x7F);

    int intact = 1;
    for (int i = 0; i < blockBytes; i++)
        if (block[i] != (char)(i & 0x7F))
            intact = 0;

    if (intact)
        print("sbrktest: heap page write and read-back verified OK\n");
    else
        print("sbrktest: heap verification FAILED\n");

    return 0;
}
