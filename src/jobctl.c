#include "jobctl.h"
#include "sched.h"
#include "vfs.h"
#include "fs.h"
#include "elf.h"
#include "paging.h"
#include "keyboard.h"
#include "pipe.h"
#include "string.h"

#define jobctlStackSize 0x4000

static uint32 jobctlSpawn(const char* path, const char* const* argv, int argc)
{
    VfsNode* node = vfsLookup(path);
    if (!node)
        return threadInvalidId;
    if (!vfsPermitted(vfsMode(node), vfsUid(node), vfsGid(node), currentThreadUid(), currentThreadGid(), fsModeOwnerExec))
        return threadInvalidId;

    const void* image = vfsFileData(node);
    uint64 size = vfsSize(node);
    if (threadImageSlotBusy(image, size, threadInvalidId))
        return threadInvalidId;
    uint64 entry = elfLoad(image, size);
    if (entry == 0)
        return threadInvalidId;

    uint64 stackTop = elfSlotStackTop(entry);
    if (!pagingMapUserRangeFlags(stackTop - jobctlStackSize, jobctlStackSize, pageNoExecute))
        return threadInvalidId;

    uint64 initialRsp;
    if (!elfBuildInitialStack(stackTop, jobctlStackSize, argv, argc, &initialRsp))
        return threadInvalidId;

    uint32 pid = threadCreateUser(path, entry, initialRsp);
    if (pid == threadInvalidId)
        return threadInvalidId;

    threadSetBrk(pid, elfSlotBrkStart(entry));

    threadSetPgid(pid, pid);
    return pid;
}

bool jobctlBinaryBusy(const char* path)
{
    VfsNode* node = vfsLookup(path);
    return node && threadImageSlotBusy(vfsFileData(node), vfsSize(node), threadInvalidId);
}

uint32 jobctlSpawnPiped(const char* path, const char* const* argv, int argc,
                         int stdinHandle, int stdoutHandle, uint32 joinPgid)
{
    uint32 pid = jobctlSpawn(path, argv, argc);
    if (pid == threadInvalidId)
        return threadInvalidId;

    if (joinPgid != threadInvalidId)
        threadSetPgid(pid, joinPgid);

    FileDescriptor* fds = threadAt(pid)->fds;
    if (stdinHandle >= 0) {
        keyboardCloseReader(fds[0].pipeHandle);
        vfsPipeAttachInto(fds, 0, stdinHandle, false);
    }
    if (stdoutHandle >= 0)
        vfsPipeAttachInto(fds, 1, stdoutHandle, true);

    return pid;
}

uint32 jobctlSpawnForeground(const char* path, const char* const* argv, int argc)
{
    uint32 pid = jobctlSpawnPiped(path, argv, argc, -1, -1, threadInvalidId);
    if (pid == threadInvalidId)
        return threadInvalidId;

    keyboardSetForegroundPgid(pid);
    return pid;
}

int64 jobctlWaitForeground(uint32 pid)
{
    uint32 callerPgid = threadPgid(currentThreadId());

    int64 code = threadJoin(pid);

    keyboardSetForegroundPgid(callerPgid);

    return code;
}

int64 jobctlWaitPipelineStages(const uint32* pids, int count)
{
    uint32 callerPgid = threadPgid(currentThreadId());

    keyboardSetForegroundPgid(pids[0]);

    int64 lastCode = 0;
    for (int i = 0; i < count; i++)
        lastCode = threadJoin(pids[i]);

    keyboardSetForegroundPgid(callerPgid);

    return lastCode;
}

bool jobctlRunBackground(const char* path, const char* const* argv, int argc,
                          uint32* outPid)
{
    uint32 pid = jobctlSpawnPiped(path, argv, argc, -1, -1, threadInvalidId);
    if (pid == threadInvalidId)
        return false;

    *outPid = pid;
    return true;
}

bool jobctlApplyRedirects(uint32 pid, const char* inPath, const char* outPath, bool outAppend,
                           const char* errPath, bool errAppend)
{
    FileDescriptor* fds = threadAt(pid)->fds;

    if (inPath) {
        keyboardCloseReader(fds[0].pipeHandle);
        if (vfsOpenInto(fds, 0, inPath, oReadOnly) < 0)
            return false;
    }

    if (outPath) {
        uint32 flags = oCreate | oWriteOnly | (outAppend ? oAppend : oTrunc);
        if (vfsOpenInto(fds, 1, outPath, flags) < 0)
            return false;
    }

    if (errPath) {
        uint32 flags = oCreate | oWriteOnly | (errAppend ? oAppend : oTrunc);
        if (vfsOpenInto(fds, 2, errPath, flags) < 0)
            return false;
    }

    return true;
}

uint32 jobctlSpawnForegroundRedirected(const char* path, const char* const* argv, int argc,
                                        const char* inPath, const char* outPath, bool outAppend,
                                        const char* errPath, bool errAppend)
{
    uint32 pid = jobctlSpawnPiped(path, argv, argc, -1, -1, threadInvalidId);
    if (pid == threadInvalidId)
        return threadInvalidId;

    if (!jobctlApplyRedirects(pid, inPath, outPath, outAppend, errPath, errAppend)) {
        threadKill(pid);
        threadJoin(pid);
        return threadInvalidId;
    }

    keyboardSetForegroundPgid(pid);
    return pid;
}

bool jobctlRunBackgroundRedirected(const char* path, const char* const* argv, int argc,
                                    const char* inPath, const char* outPath, bool outAppend,
                                    const char* errPath, bool errAppend,
                                    uint32* outPid)
{
    uint32 pid = jobctlSpawnPiped(path, argv, argc, -1, -1, threadInvalidId);
    if (pid == threadInvalidId)
        return false;

    if (!jobctlApplyRedirects(pid, inPath, outPath, outAppend, errPath, errAppend)) {
        threadKill(pid);
        threadJoin(pid);
        return false;
    }

    *outPid = pid;
    return true;
}

uint32 jobctlSpawnPipedHereString(const char* path, const char* const* argv, int argc,
                                   const char* text, int stdoutHandle, uint32 joinPgid)
{
    int handle = vfsPipeCreateHandle();
    if (handle < 0)
        return threadInvalidId;

    uint32 pid = jobctlSpawnPiped(path, argv, argc, handle, stdoutHandle, joinPgid);
    if (pid == threadInvalidId) {
        pipeDestroyIfUnattached(handle);
        return threadInvalidId;
    }

    pipeAddWriter(handle);
    uint64 length = strlen(text);
    if (length > 0)
        pipeWrite(handle, text, length);
    pipeWrite(handle, "\n", 1);
    pipeCloseWrite(handle);

    return pid;
}

uint32 jobctlSpawnForegroundHereString(const char* path, const char* const* argv, int argc,
                                        const char* text, const char* outPath, bool outAppend,
                                        const char* errPath, bool errAppend)
{
    uint32 pid = jobctlSpawnPipedHereString(path, argv, argc, text, -1, threadInvalidId);
    if (pid == threadInvalidId)
        return threadInvalidId;

    if (!jobctlApplyRedirects(pid, nullptr, outPath, outAppend, errPath, errAppend)) {
        threadKill(pid);
        threadJoin(pid);
        return threadInvalidId;
    }

    keyboardSetForegroundPgid(pid);
    return pid;
}

bool jobctlRunBackgroundHereString(const char* path, const char* const* argv, int argc,
                                    const char* text, const char* outPath, bool outAppend,
                                    const char* errPath, bool errAppend,
                                    uint32* outPid)
{
    uint32 pid = jobctlSpawnPipedHereString(path, argv, argc, text, -1, threadInvalidId);
    if (pid == threadInvalidId)
        return false;

    if (!jobctlApplyRedirects(pid, nullptr, outPath, outAppend, errPath, errAppend)) {
        threadKill(pid);
        threadJoin(pid);
        return false;
    }

    *outPid = pid;
    return true;
}

bool jobctlCaptureOutput(const char* path, const char* const* argv, int argc,
                          char* outBuf, int outCap, int64* outStatus)
{
    int handle = vfsPipeCreateHandle();
    if (handle < 0)
        return false;

    pipeAddReader(handle);

    uint32 pid = jobctlSpawnPiped(path, argv, argc, -1, handle, threadInvalidId);
    if (pid == threadInvalidId) {
        pipeCloseRead(handle);
        return false;
    }

    *outStatus = threadJoin(pid);

    int used = 0;
    char scratch[64];
    for (;;) {
        int64 n = (used < outCap - 1)
            ? pipeRead(handle, outBuf + used, (uint64)(outCap - 1 - used))
            : pipeRead(handle, scratch, sizeof(scratch));
        if (n <= 0)
            break;
        if (used < outCap - 1)
            used += (int)n;
    }
    outBuf[used] = 0;

    pipeCloseRead(handle);
    return true;
}
