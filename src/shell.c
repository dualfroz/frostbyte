#include "shell.h"
#include "console.h"
#include "keyboard.h"
#include "ports.h"
#include "vga.h"
#include "pmm.h"
#include "heap.h"
#include "pit.h"
#include "sched.h"
#include "string.h"
#include "vfs.h"
#include "fs.h"
#include "jobctl.h"
#include "klib.h"
#include "pipe.h"
#include "rtc.h"

#define lineCapacity 128
#define pathCapacity 256
#define binPathCapacity 272

#define shellExecNotFound 127

#define maxBackgroundJobs 8

typedef struct BackgroundJob {
    uint32 pid;
    char name[32];
    bool used;
} BackgroundJob;

static BackgroundJob backgroundJobs[maxBackgroundJobs];
static uint32 lastSpawnedPid = threadInvalidId;

static int64 lastStatus = 0;

uint32 shellLastSpawnedPid(void)
{
    return lastSpawnedPid;
}

static void reapFinishedBackgroundJobs(void)
{
    uint32 deadId;
    int64 code;
    while (threadJoinAnyChild(currentThreadId(), &deadId, &code) == waitScanFoundDead) {
        for (int i = 0; i < maxBackgroundJobs; i++) {
            if (backgroundJobs[i].used && backgroundJobs[i].pid == deadId) {
                kprintf("[%u] %s done, exit code %ld\n", deadId, backgroundJobs[i].name, code);
                backgroundJobs[i].used = false;
                break;
            }
        }
    }
}

static void listBackgroundJobs(void)
{
    bool any = false;
    for (int i = 0; i < maxBackgroundJobs; i++) {
        if (backgroundJobs[i].used) {
            kprintf("[%u] running  %s\n", backgroundJobs[i].pid, backgroundJobs[i].name);
            any = true;
        }
    }
    if (!any)
        kprintf("no background jobs\n");
}

int shellTokenize(char* text, char* tokens[], int maxTokens)
{
    int count = 0;
    char* cursor = text;
    while (*cursor && count < maxTokens) {
        while (*cursor == ' ')
            cursor++;
        if (*cursor == 0)
            break;
        tokens[count++] = cursor;
        while (*cursor && *cursor != ' ')
            cursor++;
        if (*cursor == ' ') {
            *cursor = 0;
            cursor++;
        }
    }
    return count;
}

int shellSplitPipelineStages(char* line, char* stages[], int maxStages)
{
    int pipeCount = 0;
    for (const char* cursor = line; *cursor; cursor++)
        if (*cursor == '|')
            pipeCount++;

    if (pipeCount == 0)
        return 0;
    if (pipeCount + 1 > maxStages)
        return -1;

    int count = 0;
    char* stageStart = line;
    char* cursor = line;
    while (true) {
        bool atPipe = (*cursor == '|');
        bool atEnd = (*cursor == 0);
        if (atPipe || atEnd) {
            char* stageEnd = cursor;
            while (stageEnd > stageStart && stageEnd[-1] == ' ')
                stageEnd--;
            *stageEnd = 0;

            stages[count++] = stageStart;

            if (atEnd)
                break;

            *cursor = 0;
            stageStart = cursor + 1;
            while (*stageStart == ' ')
                stageStart++;
        }
        cursor++;
    }

    return count;
}

int shellSplitAndOr(char* line, char* segments[], ShellAndOrOp andOrOps[], int maxSegments)
{
    int opCount = 0;
    for (const char* cursor = line; *cursor; ) {
        if ((cursor[0] == '&' && cursor[1] == '&') || (cursor[0] == '|' && cursor[1] == '|')) {
            opCount++;
            cursor += 2;
        } else {
            cursor++;
        }
    }

    if (opCount == 0)
        return 0;
    if (opCount + 1 > maxSegments)
        return -1;

    int count = 0;
    char* segStart = line;
    char* cursor = line;
    while (true) {
        bool atAnd = (cursor[0] == '&' && cursor[1] == '&');
        bool atOr = (cursor[0] == '|' && cursor[1] == '|');
        bool atEnd = (*cursor == 0);
        if (atAnd || atOr || atEnd) {
            char* segEnd = cursor;
            while (segEnd > segStart && segEnd[-1] == ' ')
                segEnd--;
            *segEnd = 0;

            segments[count++] = segStart;

            if (atEnd)
                break;

            andOrOps[count - 1] = atAnd ? shellAndOrAnd : shellAndOrOr;

            cursor += 2;
            segStart = cursor;
            while (*segStart == ' ')
                segStart++;
            cursor = segStart;
            continue;
        }
        cursor++;
    }

    return count;
}

bool shellExtractRedirects(char** argv, int* argc, ShellRedirect* out)
{
    int count = *argc;

    for (int i = 0; i < count; i++) {
        bool isOperator = strcmp(argv[i], "<") == 0 || strcmp(argv[i], "<<<") == 0 ||
                           strcmp(argv[i], ">") == 0 || strcmp(argv[i], ">>") == 0 ||
                           strcmp(argv[i], "2>") == 0 || strcmp(argv[i], "2>>") == 0;
        if (isOperator && i + 1 >= count)
            return false;
    }

    out->inPath = nullptr;
    out->outPath = nullptr;
    out->appendOut = false;
    out->errPath = nullptr;
    out->appendErr = false;
    out->hereString = nullptr;

    int writeIndex = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(argv[i], "<") == 0) {
            out->inPath = argv[++i];
            out->hereString = nullptr;
        } else if (strcmp(argv[i], "<<<") == 0) {
            out->hereString = argv[++i];
            out->inPath = nullptr;
        } else if (strcmp(argv[i], ">") == 0) {
            out->outPath = argv[++i];
            out->appendOut = false;
        } else if (strcmp(argv[i], ">>") == 0) {
            out->outPath = argv[++i];
            out->appendOut = true;
        } else if (strcmp(argv[i], "2>") == 0) {
            out->errPath = argv[++i];
            out->appendErr = false;
        } else if (strcmp(argv[i], "2>>") == 0) {
            out->errPath = argv[++i];
            out->appendErr = true;
        } else {
            argv[writeIndex++] = argv[i];
        }
    }
    *argc = writeIndex;
    return true;
}

bool shellParseVarRef(const char* token, char* nameOut, int nameCap)
{
    if (token[0] != '$' || token[1] == 0)
        return false;

    int len = 0;
    for (const char* c = token + 1; *c; c++) {
        bool isNameChar = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                           (*c >= '0' && *c <= '9') || *c == '_';
        if (!isNameChar)
            return false;
        len++;
    }
    if (len + 1 > nameCap)
        return false;

    memcpy(nameOut, token + 1, (uint64)len);
    nameOut[len] = 0;
    return true;
}

bool shellIsStatusVarToken(const char* token)
{
    return strcmp(token, "$?") == 0;
}

static const char* shellExpandToken(const char* token, char* storage, int storageCap)
{
    if (shellIsStatusVarToken(token)) {
        snprintf(storage, (uint64)storageCap, "%ld", lastStatus);
        return storage;
    }

    char name[envEntryMax];
    if (shellParseVarRef(token, name, sizeof(name))) {
        const char* value = currentThreadGetEnv(name);
        snprintf(storage, (uint64)storageCap, "%s", value ? value : "");
        return storage;
    }

    return token;
}

static void shellExpandArgv(char** argv, int argc, char storage[][envEntryMax])
{
    for (int i = 0; i < argc; i++)
        argv[i] = (char*)shellExpandToken(argv[i], storage[i], envEntryMax);
}

static void shellExpandRedirect(ShellRedirect* redirect, char storage[4][envEntryMax])
{
    if (redirect->inPath)
        redirect->inPath = shellExpandToken(redirect->inPath, storage[0], envEntryMax);
    if (redirect->outPath)
        redirect->outPath = shellExpandToken(redirect->outPath, storage[1], envEntryMax);
    if (redirect->errPath)
        redirect->errPath = shellExpandToken(redirect->errPath, storage[2], envEntryMax);
    if (redirect->hereString)
        redirect->hereString = shellExpandToken(redirect->hereString, storage[3], envEntryMax);
}

bool shellFindCommandSubst(const char* line, int* startOut, int* endOut, char* cmdOut, int cmdCap)
{
    const char* open = strstr(line, "$(");
    if (!open) {
        *startOut = -1;
        return true;
    }

    const char* close = strchr(open + 2, ')');
    if (!close)
        return false;

    int len = (int)(close - (open + 2));
    if (len > cmdCap - 1)
        len = cmdCap - 1;
    memcpy(cmdOut, open + 2, (uint64)len);
    cmdOut[len] = 0;

    *startOut = (int)(open - line);
    *endOut = (int)(close - line) + 1;
    return true;
}

bool shellGlobMatch(const char* pattern, const char* name)
{
    const char* p = pattern;
    const char* n = name;
    const char* starPattern = nullptr;
    const char* starName = nullptr;

    while (*n) {
        if (*p == '?' || *p == *n) {
            p++;
            n++;
        } else if (*p == '*') {
            starPattern = p++;
            starName = n;
        } else if (starPattern) {
            p = starPattern + 1;
            n = ++starName;
        } else {
            return false;
        }
    }
    while (*p == '*')
        p++;
    return *p == 0;
}

bool shellTokenHasGlob(const char* token)
{
    for (const char* c = token; *c; c++)
        if (*c == '*' || *c == '?')
            return true;
    return false;
}

void shellSplitGlobToken(const char* token, char* dirOut, int dirCap, char* patternOut, int patternCap)
{
    const char* lastSlash = nullptr;
    for (const char* c = token; *c; c++)
        if (*c == '/')
            lastSlash = c;

    if (!lastSlash) {
        if (dirCap > 0)
            dirOut[0] = 0;
        snprintf(patternOut, (uint64)patternCap, "%s", token);
        return;
    }

    int dirLen = (int)(lastSlash - token) + 1;
    if (dirLen >= dirCap)
        dirLen = dirCap - 1;
    memcpy(dirOut, token, (uint64)dirLen);
    dirOut[dirLen] = 0;

    snprintf(patternOut, (uint64)patternCap, "%s", lastSlash + 1);
}

#define shellGlobPathCapacity 64

static int shellExpandGlobs(char** argv, int argc, int maxArgc, char storage[][shellGlobPathCapacity])
{
    char* result[shellMaxArgs];
    int resultCount = 0;

    for (int i = 0; i < argc; i++) {
        if (!shellTokenHasGlob(argv[i])) {
            if (resultCount >= maxArgc)
                return -1;
            result[resultCount++] = argv[i];
            continue;
        }

        char dirPart[pathCapacity];
        char patternPart[pathCapacity];
        shellSplitGlobToken(argv[i], dirPart, sizeof(dirPart), patternPart, sizeof(patternPart));

        char resolvedDir[pathCapacity];
        vfsResolvePath(dirPart, resolvedDir, sizeof(resolvedDir));

        int matchStart = resultCount;
        VfsNode* dirNode = vfsLookup(resolvedDir);
        if (dirNode && vfsIsDir(dirNode)) {
            for (VfsNode* child = vfsChild(dirNode); child; child = vfsNext(child)) {
                if (!shellGlobMatch(patternPart, vfsName(child)))
                    continue;
                if (resultCount >= maxArgc)
                    return -1;

                int pos = snprintf(storage[resultCount], shellGlobPathCapacity, "%s", resolvedDir);
                if (pos < 0)
                    pos = 0;
                if (pos >= shellGlobPathCapacity)
                    pos = shellGlobPathCapacity - 1;
                snprintf(storage[resultCount] + pos, (uint64)(shellGlobPathCapacity - pos), "%s",
                         vfsName(child));

                result[resultCount] = storage[resultCount];
                resultCount++;
            }
        }

        if (resultCount == matchStart) {
            if (resultCount >= maxArgc)
                return -1;
            result[resultCount++] = argv[i];
        }
    }

    for (int i = 0; i < resultCount; i++)
        argv[i] = result[i];
    return resultCount;
}

static bool reportBusyBinary(const char* binPath, const char* name)
{
    if (!jobctlBinaryBusy(binPath))
        return false;
    kprintf("%s: already running, only one copy of a binary can be alive at a time\n", name);
    return true;
}

static int64 runProgram(char* line, char* argument)
{
    char* argv[shellMaxArgs];
    int argc = 0;
    argv[argc++] = line;
    argc += shellTokenize(argument, &argv[argc], shellMaxArgs - argc);

    char expandedArgv[shellMaxArgs][envEntryMax];
    shellExpandArgv(argv + 1, argc - 1, expandedArgv + 1);

    bool background = (argc > 1 && strcmp(argv[argc - 1], "&") == 0);
    if (background)
        argc--;

    ShellRedirect redirect;
    if (!shellExtractRedirects(argv, &argc, &redirect)) {
        kprintf("%s: redirection operator with no filename\n", line);
        return shellExecNotFound;
    }
    char redirectStorage[4][envEntryMax];
    shellExpandRedirect(&redirect, redirectStorage);

    char globStorage[shellMaxArgs][shellGlobPathCapacity];
    int expandedArgc = shellExpandGlobs(argv + 1, argc - 1, shellMaxArgs - 1, globStorage);
    if (expandedArgc < 0) {
        kprintf("%s: too many arguments after wildcard expansion\n", line);
        return shellExecNotFound;
    }
    argc = 1 + expandedArgc;

    char binPath[binPathCapacity];
    snprintf(binPath, sizeof(binPath), "/bin/%s", line);

    if (background) {
        uint32 pid;
        bool spawned = redirect.hereString
            ? jobctlRunBackgroundHereString(binPath, (const char* const*)argv, argc,
                                             redirect.hereString, redirect.outPath,
                                             redirect.appendOut, redirect.errPath,
                                             redirect.appendErr, &pid)
            : jobctlRunBackgroundRedirected(binPath, (const char* const*)argv, argc,
                                             redirect.inPath, redirect.outPath, redirect.appendOut,
                                             redirect.errPath, redirect.appendErr, &pid);
        if (!spawned) {
            if (!reportBusyBinary(binPath, line))
                kprintf("unknown command or bad redirection: %s (try 'help')\n", line);
            return shellExecNotFound;
        }
        for (int i = 0; i < maxBackgroundJobs; i++) {
            if (!backgroundJobs[i].used) {
                backgroundJobs[i].used = true;
                backgroundJobs[i].pid = pid;
                snprintf(backgroundJobs[i].name, sizeof(backgroundJobs[i].name), "%s", line);
                break;
            }
        }
        lastSpawnedPid = pid;
        kprintf("[%u] %s started in background\n", pid, line);
        return 0;
    }

    uint32 pid = redirect.hereString
        ? jobctlSpawnForegroundHereString(binPath, (const char* const*)argv, argc,
                                           redirect.hereString, redirect.outPath,
                                           redirect.appendOut, redirect.errPath, redirect.appendErr)
        : jobctlSpawnForegroundRedirected(binPath, (const char* const*)argv, argc,
                                           redirect.inPath, redirect.outPath,
                                           redirect.appendOut, redirect.errPath,
                                           redirect.appendErr);
    if (pid == threadInvalidId) {
        if (!reportBusyBinary(binPath, line))
            kprintf("unknown command or bad redirection: %s (try 'help')\n", line);
        return shellExecNotFound;
    }
    lastSpawnedPid = pid;
    int64 exitCode = jobctlWaitForeground(pid);
    kprintf("%s exited with code %ld\n", line, exitCode);
    return exitCode;
}

static void splitCommandWord(char* text, char** word, char** rest)
{
    char* cursor = text;
    while (*cursor && *cursor != ' ')
        cursor++;
    if (*cursor == ' ') {
        *cursor = 0;
        cursor++;
    }
    while (*cursor == ' ')
        cursor++;
    *word = text;
    *rest = cursor;
}

static int64 runPipelineStages(char* stages[], int count)
{
    char* word[shellMaxPipelineStages];
    char* argv[shellMaxPipelineStages][shellMaxArgs];
    int argc[shellMaxPipelineStages];
    ShellRedirect redirect[shellMaxPipelineStages];
    char expanded[shellMaxPipelineStages][shellMaxArgs][envEntryMax];
    char globStorage[shellMaxPipelineStages][shellMaxArgs][shellGlobPathCapacity];
    char redirectStorage[shellMaxPipelineStages][4][envEntryMax];

    for (int i = 0; i < count; i++) {
        char* rest;
        splitCommandWord(stages[i], &word[i], &rest);
        argc[i] = 0;
        argv[i][argc[i]++] = word[i];
        argc[i] += shellTokenize(rest, &argv[i][argc[i]], shellMaxArgs - argc[i]);
        shellExpandArgv(&argv[i][1], argc[i] - 1, &expanded[i][1]);

        if (!shellExtractRedirects(argv[i], &argc[i], &redirect[i])) {
            kprintf("%s: redirection operator with no filename\n", word[i]);
            return shellExecNotFound;
        }
        shellExpandRedirect(&redirect[i], redirectStorage[i]);

        int expandedArgc = shellExpandGlobs(&argv[i][1], argc[i] - 1, shellMaxArgs - 1, globStorage[i]);
        if (expandedArgc < 0) {
            kprintf("%s: too many arguments after wildcard expansion\n", word[i]);
            return shellExecNotFound;
        }
        argc[i] = 1 + expandedArgc;
    }

    for (int i = 0; i < count; i++) {
        if (i > 0 && redirect[i].inPath) {
            kprintf("pipeline: '%s' already reads from the pipe, input redirection is only "
                    "supported on the first stage\n", word[i]);
            return shellExecNotFound;
        }
        if (i > 0 && redirect[i].hereString) {
            kprintf("pipeline: '%s' already reads from the pipe, here-string redirection is "
                    "only supported on the first stage\n", word[i]);
            return shellExecNotFound;
        }
        if (i < count - 1 && redirect[i].outPath) {
            kprintf("pipeline: '%s' already writes to the pipe, output redirection is only "
                    "supported on the last stage\n", word[i]);
            return shellExecNotFound;
        }
    }

    uint32 pids[shellMaxPipelineStages];
    int pipeHandle[shellMaxPipelineStages - 1];

    for (int i = 0; i < count; i++) {
        int stdinHandle = (i == 0) ? -1 : pipeHandle[i - 1];
        int stdoutHandle = -1;
        if (i < count - 1) {
            pipeHandle[i] = vfsPipeCreateHandle();
            if (pipeHandle[i] < 0) {
                kprintf("pipeline: no pipe slots free\n");
                for (int k = 0; k < i; k++) {
                    threadKill(pids[k]);
                    threadJoin(pids[k]);
                }
                return shellExecNotFound;
            }
            stdoutHandle = pipeHandle[i];
        }

        uint32 joinPgid = (i == 0) ? threadInvalidId : pids[0];

        char binPath[binPathCapacity];
        snprintf(binPath, sizeof(binPath), "/bin/%s", word[i]);

        uint32 pid = (i == 0 && redirect[i].hereString)
            ? jobctlSpawnPipedHereString(binPath, (const char* const*)argv[i], argc[i],
                                          redirect[i].hereString, stdoutHandle, joinPgid)
            : jobctlSpawnPiped(binPath, (const char* const*)argv[i], argc[i],
                                stdinHandle, stdoutHandle, joinPgid);

        if (pid == threadInvalidId) {
            if (!reportBusyBinary(binPath, word[i]))
                kprintf("unknown command: %s (try 'help')\n", word[i]);
            if (i < count - 1)
                pipeDestroyIfUnattached(pipeHandle[i]);
            for (int k = 0; k < i; k++) {
                threadKill(pids[k]);
                threadJoin(pids[k]);
            }
            return shellExecNotFound;
        }
        pids[i] = pid;

        const char* inPath = (i == 0) ? redirect[i].inPath : nullptr;
        const char* outPath = (i == count - 1) ? redirect[i].outPath : nullptr;
        bool appendOut = (i == count - 1) ? redirect[i].appendOut : false;
        if ((inPath || outPath || redirect[i].errPath) &&
            !jobctlApplyRedirects(pid, inPath, outPath, appendOut,
                                   redirect[i].errPath, redirect[i].appendErr)) {
            kprintf("%s: cannot open %s\n", word[i],
                    inPath ? inPath : (outPath ? outPath : redirect[i].errPath));
            for (int k = 0; k <= i; k++) {
                threadKill(pids[k]);
                threadJoin(pids[k]);
            }
            return shellExecNotFound;
        }
    }

    lastSpawnedPid = pids[count - 1];
    int64 exitCode = jobctlWaitPipelineStages(pids, count);

    char summary[lineCapacity];
    int pos = 0;
    for (int i = 0; i < count; i++) {
        if (i > 0)
            pos += snprintf(summary + pos, sizeof(summary) - pos, " | ");
        pos += snprintf(summary + pos, sizeof(summary) - pos, "%s", word[i]);
    }
    kprintf("%s exited with code %ld\n", summary, exitCode);
    return exitCode;
}

static void printHelp(void)
{
    kprintf("available commands:\n");
    kprintf("  help          show this list\n");
    kprintf("  echo TEXT     print the given text\n");
    kprintf("  echo TEXT >F  write the text to file F\n");
    kprintf("  ls [-l] [PATH] list a directory (tmpfs or /mnt), -l adds type/size/mtime\n");
    kprintf("  cat FILE      print a file\n");
    kprintf("  mkdir PATH    create a directory (writable mounts only)\n");
    kprintf("  rm PATH       remove a file (refuses a directory)\n");
    kprintf("  rmdir PATH    remove an empty directory\n");
    kprintf("  chmod MODE PATH  set owner/group/other rwx bits (octal, e.g. 644 or 750)\n");
    kprintf("  chgrp GID PATH   set group (decimal); owner may only set its own gid\n");
    kprintf("  cd PATH       change the working directory\n");
    kprintf("  pwd           print the working directory\n");
    kprintf("  whoami        print this shell's own uid (0 = root)\n");
    kprintf("  mem           physical memory and heap usage\n");
    kprintf("  uptime        ticks and seconds since boot\n");
    kprintf("  ps            list kernel threads\n");
    kprintf("  jobs          list running background jobs\n");
    kprintf("  export N=V    set environment variable N to V (inherited by children)\n");
    kprintf("  unset N       remove environment variable N\n");
    kprintf("  env           list this shell's environment variables\n");
    kprintf("  clear         clear the screen\n");
    kprintf("  $NAME         (in a spawned program's arguments) expands to that variable\n");
    kprintf("  $?            (same place) expands to the last command's exit status\n");
    kprintf("  NAME dir/*.ext (glob) expands to every real matching filename in dir\n");
    kprintf("  NAME [ARGS]   run /bin/NAME in the foreground, waits for it to exit\n");
    kprintf("  NAME [ARGS] & run /bin/NAME in the background, prints its pid\n");
    kprintf("  NAME < IN     redirect NAME's stdin from file IN\n");
    kprintf("  NAME <<< TEXT feed TEXT (plus a newline) to NAME's stdin\n");
    kprintf("  NAME > OUT    redirect NAME's stdout to file OUT (create/truncate)\n");
    kprintf("  NAME >> OUT   redirect NAME's stdout to file OUT (create/append)\n");
    kprintf("  NAME 2> ERR   redirect NAME's stderr to file ERR (create/truncate)\n");
    kprintf("  NAME 2>> ERR  redirect NAME's stderr to file ERR (create/append)\n");
    kprintf("  A [ARGS] | B [ARGS]  pipe A's stdout into B's stdin, waits for both\n");
}

static void printMem(void)
{
    uint64 totalKib = pmmTotalBytes() / 1024;
    uint64 usedKib = pmmUsedFrames() * frameSize / 1024;
    uint64 freeKib = pmmFreeFrames() * frameSize / 1024;

    kprintf("physical memory: %lu KiB total\n", totalKib);
    kprintf("  frames: %lu total, %lu used, %lu free\n",
            pmmTotalFrames(), pmmUsedFrames(), pmmFreeFrames());
    kprintf("  used %lu KiB, free %lu KiB\n", usedKib, freeKib);
    kprintf("heap: %lu KiB used of %lu KiB\n",
            heapUsed() / 1024, heapTotal() / 1024);
}

static void printUptime(void)
{
    uint64 ticks = pitTicks();
    uint32 frequency = pitFrequency();
    kprintf("uptime: %lu ticks, %lu seconds (at %u Hz)\n",
            ticks, ticks / frequency, frequency);
}

static const char* threadStateName(ThreadState state)
{
    switch (state) {
    case threadUnused:   return "unused";
    case threadRunnable: return "runnable";
    case threadRunning:  return "running";
    case threadSleeping: return "sleeping";
    case threadDead:     return "dead";
    case threadBlocked:  return "blocked";
    case threadStopped:  return "stopped";
    default:             return "?";
    }
}

static void printThreads(void)
{
    kprintf("id  state     name\n");
    for (uint32 i = 0; i < threadCount(); i++) {
        Thread* thread = threadAt(i);
        const char* state = threadStateName(thread->state);
        char marker = thread->id == currentThreadId() ? '*' : ' ';
        kprintf("%c%u   %s  %s\n", marker, thread->id, state, thread->name);
    }
}

static void buildPath(const char* argument, char* out)
{
    vfsResolvePath(argument, out, pathCapacity);
}

static void printLsEntry(bool longFormat, const FbStat* stat, const char* name)
{
    char typeChar = stat->type == fsTypeDir ? 'd' : (stat->type == fsTypeFifo ? 'p' : 'f');

    if (!longFormat) {
        if (stat->type == fsTypeDir)
            kprintf("%s/\n", name);
        else
            kprintf("%s  %lu\n", name, stat->size);
        return;
    }

    char perm[10];
    perm[0] = (stat->mode & fsModeOwnerRead) ? 'r' : '-';
    perm[1] = (stat->mode & fsModeOwnerWrite) ? 'w' : '-';
    perm[2] = (stat->mode & fsModeOwnerExec) ? 'x' : '-';
    perm[3] = (stat->mode & fsModeGroupRead) ? 'r' : '-';
    perm[4] = (stat->mode & fsModeGroupWrite) ? 'w' : '-';
    perm[5] = (stat->mode & fsModeGroupExec) ? 'x' : '-';
    perm[6] = (stat->mode & fsModeOtherRead) ? 'r' : '-';
    perm[7] = (stat->mode & fsModeOtherWrite) ? 'w' : '-';
    perm[8] = (stat->mode & fsModeOtherExec) ? 'x' : '-';
    perm[9] = 0;

    kprintf("%c %lu %s ", typeChar, stat->size, name);
    if (stat->mtime == 0) {
        kprintf("- %s\n", perm);
    } else {
        RtcDateTime dt;
        rtcCivilFromEpoch((int64)stat->mtime, &dt);
        kprintf("%04ld-%02ld-%02ld %02ld:%02ld:%02ld %s\n",
                dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second, perm);
    }
}

static int listDir(const char* argument)
{
    bool longFormat = false;
    if (argument[0] == '-' && argument[1] == 'l' && (argument[2] == 0 || argument[2] == ' ')) {
        longFormat = true;
        argument += 2;
        while (*argument == ' ')
            argument++;
    }

    char path[pathCapacity];
    buildPath(argument, path);

    FbStat stat;
    if (vfsStat(path, &stat) != 0) {
        kprintf("ls: no such path: %s\n", path);
        return 1;
    }

    if (stat.type != fsTypeDir) {
        const char* base = path;
        for (const char* p = path; *p; p++) {
            if (*p == '/')
                base = p + 1;
        }
        printLsEntry(longFormat, &stat, base);
        return 0;
    }

    int fd = vfsOpen(path, oReadOnly);
    if (fd < 0) {
        kprintf("ls: cannot open %s\n", path);
        return 1;
    }

    uint64 pathLen = strlen(path);
    if (pathLen >= pathCapacity)
        pathLen = pathCapacity - 1;
    bool needSlash = pathLen == 0 || path[pathLen - 1] != '/';

    FbDirent entries[16];
    int64 bytes;
    while ((bytes = vfsGetdents(fd, entries, sizeof(entries))) > 0) {
        int64 count = bytes / (int64)sizeof(FbDirent);
        for (int64 i = 0; i < count; i++) {
            char childPath[pathCapacity];
            uint64 childLen = pathLen;
            memcpy(childPath, path, childLen);
            if (needSlash && childLen < pathCapacity - 1)
                childPath[childLen++] = '/';
            uint64 nameLen = strlen(entries[i].name);
            if (childLen + nameLen >= pathCapacity)
                nameLen = pathCapacity - 1 - childLen;
            memcpy(childPath + childLen, entries[i].name, nameLen);
            childPath[childLen + nameLen] = 0;

            FbStat childStat;
            if (vfsStat(childPath, &childStat) == 0)
                printLsEntry(longFormat, &childStat, entries[i].name);
            else
                kprintf("%s  ?\n", entries[i].name);
        }
    }
    vfsClose(fd);
    return 0;
}

static int catFile(const char* argument)
{
    if (argument[0] == 0) {
        kprintf("cat: missing file\n");
        return 1;
    }
    char path[pathCapacity];
    buildPath(argument, path);

    VfsNode* node = vfsLookup(path);
    if (!node) {
        kprintf("cat: no such file: %s\n", path);
        return 1;
    }
    if (vfsIsDir(node)) {
        kprintf("cat: %s is a directory\n", path);
        return 1;
    }
    const char* data = (const char*)vfsFileData(node);
    uint64 size = vfsSize(node);
    for (uint64 i = 0; i < size; i++)
        kputchar(data[i]);
    return 0;
}

static void echoWrite(char* text, const char* target)
{
    char path[pathCapacity];
    buildPath(target, path);

    int fd = vfsOpen(path, oCreate | oWriteOnly | oTrunc);
    if (fd < 0) {
        kprintf("echo: cannot write %s\n", path);
        return;
    }
    uint64 length = strlen(text);
    vfsWrite(fd, text, length);
    vfsWrite(fd, "\n", 1);
    vfsClose(fd);
}

static int makeDir(const char* argument)
{
    if (argument[0] == 0) {
        kprintf("mkdir: missing path\n");
        return 1;
    }
    char path[pathCapacity];
    buildPath(argument, path);
    if (vfsMkdir(path) != 0) {
        kprintf("mkdir: cannot create %s\n", path);
        return 1;
    }
    return 0;
}

static int removeFile(const char* argument)
{
    if (argument[0] == 0) {
        kprintf("rm: missing path\n");
        return 1;
    }
    char path[pathCapacity];
    buildPath(argument, path);

    FbStat stat;
    if (vfsStat(path, &stat) != 0) {
        kprintf("rm: no such file: %s\n", path);
        return 1;
    }
    if (stat.type == fsTypeDir) {
        kprintf("rm: %s: is a directory\n", path);
        return 1;
    }
    if (vfsUnlink(path) != 0) {
        kprintf("rm: cannot remove %s\n", path);
        return 1;
    }
    return 0;
}

static int removeDir(const char* argument)
{
    if (argument[0] == 0) {
        kprintf("rmdir: missing path\n");
        return 1;
    }
    char path[pathCapacity];
    buildPath(argument, path);

    FbStat stat;
    if (vfsStat(path, &stat) != 0) {
        kprintf("rmdir: no such directory: %s\n", path);
        return 1;
    }
    if (stat.type != fsTypeDir) {
        kprintf("rmdir: %s: not a directory\n", path);
        return 1;
    }
    if (vfsRmdir(path) != 0) {
        kprintf("rmdir: %s: not empty\n", path);
        return 1;
    }
    return 0;
}

static int doChmod(const char* argument)
{
    char modeText[16];
    uint64 i = 0;
    while (argument[i] && argument[i] != ' ' && i < sizeof(modeText) - 1) {
        modeText[i] = argument[i];
        i++;
    }
    modeText[i] = 0;

    const char* rest = argument + i;
    while (*rest == ' ')
        rest++;

    if (modeText[0] == 0 || rest[0] == 0) {
        kprintf("chmod: usage: chmod MODE PATH\n");
        return 1;
    }

    uint32 mode = (uint32)strtoul(modeText, nullptr, 8);
    char path[pathCapacity];
    buildPath(rest, path);
    if (vfsChmod(path, mode) != 0) {
        kprintf("chmod: cannot chmod %s\n", path);
        return 1;
    }
    return 0;
}

static int doChown(const char* argument)
{
    char uidText[16];
    uint64 i = 0;
    while (argument[i] && argument[i] != ' ' && i < sizeof(uidText) - 1) {
        uidText[i] = argument[i];
        i++;
    }
    uidText[i] = 0;

    const char* rest = argument + i;
    while (*rest == ' ')
        rest++;

    if (uidText[0] == 0 || rest[0] == 0) {
        kprintf("chown: usage: chown UID PATH\n");
        return 1;
    }

    uint32 uid = (uint32)strtoul(uidText, nullptr, 10);
    char path[pathCapacity];
    buildPath(rest, path);
    if (vfsChown(path, uid) != 0) {
        kprintf("chown: cannot chown %s\n", path);
        return 1;
    }
    return 0;
}

static int doChgrp(const char* argument)
{
    char gidText[16];
    uint64 i = 0;
    while (argument[i] && argument[i] != ' ' && i < sizeof(gidText) - 1) {
        gidText[i] = argument[i];
        i++;
    }
    gidText[i] = 0;

    const char* rest = argument + i;
    while (*rest == ' ')
        rest++;

    if (gidText[0] == 0 || rest[0] == 0) {
        kprintf("chgrp: usage: chgrp GID PATH\n");
        return 1;
    }

    uint32 gid = (uint32)strtoul(gidText, nullptr, 10);
    char path[pathCapacity];
    buildPath(rest, path);
    if (vfsChgrp(path, gid) != 0) {
        kprintf("chgrp: cannot chgrp %s\n", path);
        return 1;
    }
    return 0;
}

static int changeDir(const char* argument)
{
    char path[pathCapacity];
    if (argument[0] == 0)
        memcpy(path, "/", 2);
    else
        buildPath(argument, path);

    if (vfsChdir(path) != 0) {
        kprintf("cd: no such directory: %s\n", path);
        return 1;
    }
    return 0;
}

static int doExport(char* argument)
{
    char* eq = argument;
    while (*eq && *eq != '=')
        eq++;
    if (*eq != '=' || eq == argument) {
        kprintf("export: usage: export NAME=VALUE\n");
        return 1;
    }
    *eq = 0;
    const char* value = eq + 1;
    if (!currentThreadSetEnv(argument, value)) {
        kprintf("export: cannot set %s (name/value too long, or too many variables)\n", argument);
        return 1;
    }
    return 0;
}

static int doUnset(char* argument)
{
    if (argument[0] == 0) {
        kprintf("unset: usage: unset NAME\n");
        return 1;
    }
    currentThreadUnsetEnv(argument);
    return 0;
}

static void printEnv(void)
{
    int count = currentThreadEnvCount();
    for (int i = 0; i < count; i++)
        kprintf("env: %s\n", currentThreadEnvEntry(i));
}

static void doEcho(char* argument)
{
    char* redirect = argument;
    while (*redirect && *redirect != '>')
        redirect++;

    if (*redirect == '>') {
        *redirect = 0;
        char* target = redirect + 1;
        while (*target == ' ')
            target++;

        char* textEnd = redirect;
        while (textEnd > argument && textEnd[-1] == ' ')
            textEnd--;
        *textEnd = 0;

        echoWrite(argument, target);
    } else {
        kprintf("%s\n", argument);
    }
}

#define shellMaxCommandSubst 4

static bool shellExpandCommandSubst(const char* srcLine, char* outLine, int outCap)
{
    char work[lineCapacity];
    snprintf(work, sizeof(work), "%s", srcLine);

    int scanFrom = 0;

    for (int iterations = 0; iterations < shellMaxCommandSubst; iterations++) {
        int start, end;
        char cmd[lineCapacity];
        if (!shellFindCommandSubst(work + scanFrom, &start, &end, cmd, sizeof(cmd))) {
            kprintf("shell: '$(' with no matching ')'\n");
            return false;
        }
        if (start < 0) {
            snprintf(outLine, (uint64)outCap, "%s", work);
            return true;
        }
        start += scanFrom;
        end += scanFrom;

        char* word;
        char* rest;
        splitCommandWord(cmd, &word, &rest);

        char* argv[shellMaxArgs];
        int argc = 0;
        argv[argc++] = word;
        argc += shellTokenize(rest, &argv[argc], shellMaxArgs - argc);

        char expandedArgv[shellMaxArgs][envEntryMax];
        shellExpandArgv(argv + 1, argc - 1, expandedArgv + 1);

        char binPath[binPathCapacity];
        snprintf(binPath, sizeof(binPath), "/bin/%s", word);

        char captured[lineCapacity];
        int64 status;
        bool spawned = jobctlCaptureOutput(binPath, (const char* const*)argv, argc,
                                            captured, sizeof(captured), &status);
        (void)status;
        if (!spawned) {
            if (!reportBusyBinary(binPath, word))
                kprintf("%s: command substitution failed (unknown command)\n", word);
            return false;
        }

        int capturedLen = (int)strlen(captured);
        while (capturedLen > 0 && captured[capturedLen - 1] == '\n')
            captured[--capturedLen] = 0;

        int tailLen = (int)strlen(work + end);
        if (start + capturedLen + tailLen + 1 > lineCapacity) {
            kprintf("shell: command substitution result too long\n");
            return false;
        }

        memmove(work + start + capturedLen, work + end, (uint64)(tailLen + 1));
        memcpy(work + start, captured, (uint64)capturedLen);

        scanFrom = start + capturedLen;
    }

    kprintf("shell: too many '$(' substitutions (max %d)\n", shellMaxCommandSubst);
    return false;
}

static int64 executeSingle(char* line)
{
    if (line[0] == 0)
        return 0;

    char* pipeStages[shellMaxPipelineStages];
    int pipeStageCount = shellSplitPipelineStages(line, pipeStages, shellMaxPipelineStages);
    if (pipeStageCount == -1) {
        kprintf("pipeline: too many '|' stages (max %d)\n", shellMaxPipelineStages);
        return shellExecNotFound;
    }
    if (pipeStageCount > 0)
        return runPipelineStages(pipeStages, pipeStageCount);

    char expandedLine[lineCapacity];
    if (!shellExpandCommandSubst(line, expandedLine, sizeof(expandedLine)))
        return shellExecNotFound;
    line = expandedLine;

    char* argument = line;
    while (*argument && *argument != ' ')
        argument++;
    if (*argument == ' ') {
        *argument = 0;
        argument++;
    }
    while (*argument == ' ')
        argument++;

    if (strcmp(line, "help") == 0) {
        printHelp();
        return 0;
    } else if (strcmp(line, "echo") == 0) {
        doEcho(argument);
        return 0;
    } else if (strcmp(line, "ls") == 0)
        return listDir(argument);
    else if (strcmp(line, "cat") == 0)
        return catFile(argument);
    else if (strcmp(line, "mkdir") == 0)
        return makeDir(argument);
    else if (strcmp(line, "rm") == 0)
        return removeFile(argument);
    else if (strcmp(line, "rmdir") == 0)
        return removeDir(argument);
    else if (strcmp(line, "chmod") == 0)
        return doChmod(argument);
    else if (strcmp(line, "chown") == 0)
        return doChown(argument);
    else if (strcmp(line, "chgrp") == 0)
        return doChgrp(argument);
    else if (strcmp(line, "cd") == 0)
        return changeDir(argument);
    else if (strcmp(line, "pwd") == 0) {
        char path[pathCapacity];
        vfsGetcwd(path, sizeof(path));
        kprintf("%s\n", path);
        return 0;
    }
    else if (strcmp(line, "whoami") == 0) {
        kprintf("%u\n", currentThreadUid());
        return 0;
    }
    else if (strcmp(line, "mem") == 0) {
        printMem();
        return 0;
    } else if (strcmp(line, "uptime") == 0) {
        printUptime();
        return 0;
    } else if (strcmp(line, "ps") == 0) {
        printThreads();
        return 0;
    } else if (strcmp(line, "jobs") == 0) {
        listBackgroundJobs();
        return 0;
    } else if (strcmp(line, "export") == 0)
        return doExport(argument);
    else if (strcmp(line, "env") == 0) {
        printEnv();
        return 0;
    } else if (strcmp(line, "unset") == 0)
        return doUnset(argument);
    else if (strcmp(line, "clear") == 0) {
        vgaClear();
        return 0;
    } else
        return runProgram(line, argument);
}

static void execute(char* line)
{
    if (line[0] == 0)
        return;

    char* andOrSegments[shellMaxAndOrSegments];
    ShellAndOrOp andOrOps[shellMaxAndOrSegments - 1];
    int andOrCount = shellSplitAndOr(line, andOrSegments, andOrOps, shellMaxAndOrSegments);
    if (andOrCount == -1) {
        kprintf("shell: too many '&&'/'||' segments (max %d)\n", shellMaxAndOrSegments);
        return;
    }
    if (andOrCount == 0) {
        lastStatus = executeSingle(line);
        return;
    }

    int64 status = 0;
    for (int i = 0; i < andOrCount; i++) {
        if (i > 0) {
            bool shouldRun = (andOrOps[i - 1] == shellAndOrAnd) ? (status == 0) : (status != 0);
            if (!shouldRun)
                continue;
        }
        status = executeSingle(andOrSegments[i]);
        lastStatus = status;
    }
}

void shellRun(void)
{
    char line[lineCapacity];
    uint32 length = 0;

    int reader = keyboardOpenReader();
    if (reader == consoleReaderInvalid) {
        kprintf("[shell] console reader table full, shell has no input\n");
        for (;;)
            halt();
    }

    kprintf("\nfrostbyte shell ready. type 'help'.\n");
    kprintf("frostbyte> ");

    for (;;) {
        char c;
        keyboardReaderReadBlocking(reader, &c, 1);

        if (c == '\n') {
            kputchar('\n');
            line[length] = 0;
            execute(line);
            length = 0;
            reapFinishedBackgroundJobs();
            kprintf("frostbyte> ");
        } else if (c == '\b') {
            if (length > 0) {
                length--;
                kprintf("\b \b");
            }
        } else if (length < lineCapacity - 1) {
            line[length++] = c;
            kputchar(c);
        }
    }
}
