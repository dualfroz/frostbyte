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
#define fsTypeFifo 3

#define fsModeOwnerRead  0400u
#define fsModeOwnerWrite 0200u
#define fsModeOwnerExec  0100u

#define fsModeGroupRead  0040u
#define fsModeGroupWrite 0020u
#define fsModeGroupExec  0010u

#define fsModeOtherRead  0004u
#define fsModeOtherWrite 0002u
#define fsModeOtherExec  0001u

#define fsModeDefaultFile 0644u
#define fsModeDefaultDir  0755u
#define fsModeDefaultExec 0755u

#define mmapProtNone  0x0
#define mmapProtRead  0x1
#define mmapProtWrite 0x2
#define mmapProtExec  0x4

#define mmapAnonymous 0x1

#define fsNameMax 64

#define fsPathMax 256

typedef struct FbStat {
    unsigned long size;
    unsigned int type;
    unsigned int mode;
    unsigned int uid;
    unsigned int gid;
    unsigned long mtime;
    unsigned long ctime;
} FbStat;

typedef struct FbDirent {
    unsigned int type;
    unsigned int nameLength;
    char name[fsNameMax];
} FbDirent;

#endif
