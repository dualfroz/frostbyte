#ifndef FROSTBYTE_FS_H
#define FROSTBYTE_FS_H

#define oReadOnly  0x0
#define oWriteOnly 0x1
#define oReadWrite 0x2
#define oCreate    0x40
#define oTrunc     0x200
#define oAppend    0x400

#define seekSet 0
#define seekCur 1
#define seekEnd 2

#define fsTypeFile 1
#define fsTypeDir  2

#define fsNameMax 64

typedef struct FbStat {
    unsigned long size;
    unsigned int type;
    unsigned int mode;
} FbStat;

typedef struct FbDirent {
    unsigned int type;
    unsigned int nameLength;
    char name[fsNameMax];
} FbDirent;

#endif
