#ifndef FROSTBYTE_PIPE_H
#define FROSTBYTE_PIPE_H

#include "types.h"

#define maxPipes 32
#define pipeBufferSize 256

int pipeCreate(void);

int pipeCreateDetached(void);

void pipeAddReader(int handle);
void pipeAddWriter(int handle);

bool pipeWriteBroken(int handle);
bool pipeHasReader(int handle);

const void* pipeChannel(int handle);

bool pipeValid(int handle);

uint32 pipeIncarnation(int handle);

int64 pipeRead(int handle, void* buffer, uint64 length);
int64 pipeWrite(int handle, const void* buffer, uint64 length);

bool pipeHasData(int handle);
bool pipeHasSpace(int handle);

int64 pipeReadBlocking(int handle, void* buffer, uint64 length);

int64 pipeWriteBlocking(int handle, const void* buffer, uint64 length);

void pipeCloseRead(int handle);
void pipeCloseWrite(int handle);

bool pipeDestroyIfUnattached(int handle);

#endif
