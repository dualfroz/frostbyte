#ifndef FROSTBYTE_SHELL_H
#define FROSTBYTE_SHELL_H

#include "types.h"

void shellRun(void);

#define shellMaxArgs 16

#define shellMaxPipelineStages 4

int shellTokenize(char* text, char* tokens[], int maxTokens);

int shellSplitPipelineStages(char* line, char* stages[], int maxStages);

#define shellMaxAndOrSegments 4

typedef enum {
    shellAndOrAnd = 1,
    shellAndOrOr = 2
} ShellAndOrOp;

int shellSplitAndOr(char* line, char* segments[], ShellAndOrOp andOrOps[], int maxSegments);

typedef struct ShellRedirect {
    const char* inPath;
    const char* outPath;
    bool appendOut;
    const char* errPath;
    bool appendErr;
    const char* hereString;
} ShellRedirect;

bool shellExtractRedirects(char** argv, int* argc, ShellRedirect* out);

bool shellParseVarRef(const char* token, char* nameOut, int nameCap);

bool shellIsStatusVarToken(const char* token);

bool shellGlobMatch(const char* pattern, const char* name);

bool shellTokenHasGlob(const char* token);

void shellSplitGlobToken(const char* token, char* dirOut, int dirCap, char* patternOut, int patternCap);

bool shellFindCommandSubst(const char* line, int* startOut, int* endOut, char* cmdOut, int cmdCap);

uint32 shellLastSpawnedPid(void);

#endif
