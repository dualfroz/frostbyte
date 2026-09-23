#ifndef FROSTBYTE_JOBCTL_H
#define FROSTBYTE_JOBCTL_H

#include "types.h"

bool jobctlBinaryBusy(const char* path);

uint32 jobctlSpawnForeground(const char* path, const char* const* argv, int argc);

int64 jobctlWaitForeground(uint32 pid);

bool jobctlRunBackground(const char* path, const char* const* argv, int argc,
                          uint32* outPid);

uint32 jobctlSpawnPiped(const char* path, const char* const* argv, int argc,
                         int stdinHandle, int stdoutHandle, uint32 joinPgid);

int64 jobctlWaitPipelineStages(const uint32* pids, int count);

bool jobctlApplyRedirects(uint32 pid, const char* inPath, const char* outPath, bool outAppend,
                           const char* errPath, bool errAppend);

uint32 jobctlSpawnForegroundRedirected(const char* path, const char* const* argv, int argc,
                                        const char* inPath, const char* outPath, bool outAppend,
                                        const char* errPath, bool errAppend);

bool jobctlRunBackgroundRedirected(const char* path, const char* const* argv, int argc,
                                    const char* inPath, const char* outPath, bool outAppend,
                                    const char* errPath, bool errAppend,
                                    uint32* outPid);

uint32 jobctlSpawnPipedHereString(const char* path, const char* const* argv, int argc,
                                   const char* text, int stdoutHandle, uint32 joinPgid);

uint32 jobctlSpawnForegroundHereString(const char* path, const char* const* argv, int argc,
                                        const char* text, const char* outPath, bool outAppend,
                                        const char* errPath, bool errAppend);

bool jobctlRunBackgroundHereString(const char* path, const char* const* argv, int argc,
                                    const char* text, const char* outPath, bool outAppend,
                                    const char* errPath, bool errAppend,
                                    uint32* outPid);

bool jobctlCaptureOutput(const char* path, const char* const* argv, int argc,
                          char* outBuf, int outCap, int64* outStatus);

#endif
