#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct {
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
} UstarHeader;

static void setOctal(char* field, int width, unsigned long value)
{
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%0*lo", width - 1, value);
    memcpy(field, tmp, (size_t)(width - 1));
    field[width - 1] = '\0';
}

static void writeChecksum(UstarHeader* header)
{
    memset(header->checksum, ' ', 8);
    unsigned int sum = 0;
    unsigned char* bytes = (unsigned char*)header;
    for (int i = 0; i < 512; i++)
        sum += bytes[i];
    char tmp[8];
    snprintf(tmp, sizeof tmp, "%06o", sum & 0777777u);
    memcpy(header->checksum, tmp, 6);
    header->checksum[6] = '\0';
    header->checksum[7] = ' ';
}

static void emitHeader(FILE* out, const char* name, char typeflag, unsigned long size)
{
    UstarHeader header;
    memset(&header, 0, sizeof header);
    strncpy(header.name, name, sizeof header.name - 1);
    setOctal(header.mode, 8, 0644);
    setOctal(header.uid, 8, 0);
    setOctal(header.gid, 8, 0);
    setOctal(header.size, 12, size);
    setOctal(header.mtime, 12, 0);
    header.typeflag = typeflag;
    memcpy(header.magic, "ustar", 5);
    header.magic[5] = '\0';
    header.version[0] = '0';
    header.version[1] = '0';
    writeChecksum(&header);
    fwrite(&header, 1, 512, out);
}

static void padTo512(FILE* out, unsigned long size)
{
    unsigned long remainder = size % 512;
    if (remainder)
        for (unsigned long i = 0; i < 512 - remainder; i++)
            fputc(0, out);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s out.tar [--dir apath | --file apath hostpath]...\n", argv[0]);
        return 1;
    }

    FILE* out = fopen(argv[1], "wb");
    if (!out) {
        fprintf(stderr, "mktar: cannot write %s\n", argv[1]);
        return 1;
    }

    int i = 2;
    while (i < argc) {
        if (strcmp(argv[i], "--dir") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "mktar: --dir needs a path\n");
                return 1;
            }
            char namebuf[101];
            strncpy(namebuf, argv[i + 1], 99);
            namebuf[99] = 0;
            size_t l = strlen(namebuf);
            if (l == 0 || namebuf[l - 1] != '/') {
                namebuf[l] = '/';
                namebuf[l + 1] = 0;
            }
            emitHeader(out, namebuf, '5', 0);
            i += 2;
        } else if (strcmp(argv[i], "--file") == 0) {
            if (i + 2 >= argc) {
                fprintf(stderr, "mktar: --file needs apath hostpath\n");
                return 1;
            }
            const char* apath = argv[i + 1];
            const char* hostpath = argv[i + 2];

            FILE* in = fopen(hostpath, "rb");
            if (!in) {
                fprintf(stderr, "mktar: cannot open %s\n", hostpath);
                return 1;
            }
            fseek(in, 0, SEEK_END);
            long size = ftell(in);
            fseek(in, 0, SEEK_SET);
            if (size < 0)
                size = 0;

            unsigned char* buffer = malloc(size > 0 ? (size_t)size : 1);
            if (size > 0 && fread(buffer, 1, (size_t)size, in) != (size_t)size) {
                fprintf(stderr, "mktar: short read on %s\n", hostpath);
                return 1;
            }
            fclose(in);

            emitHeader(out, apath, '0', (unsigned long)size);
            if (size > 0)
                fwrite(buffer, 1, (size_t)size, out);
            padTo512(out, (unsigned long)size);
            free(buffer);
            i += 3;
        } else {
            fprintf(stderr, "mktar: unknown argument %s\n", argv[i]);
            return 1;
        }
    }

    for (int block = 0; block < 1024; block++)
        fputc(0, out);
    fclose(out);
    return 0;
}
