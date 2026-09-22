#include "ulib.h"

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++) {
        print("argecho: ");
        print(argv[i]);
        print("\n");
    }
    return 0;
}
