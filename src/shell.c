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

/* Real shell "command could not even be started" exit status (bad
   redirection, wildcard overflow, unknown binary, ...) -- the same 127
   real shells report for "command not found", reused here for every
   pre-spawn failure in runProgram/runPipelineStages so shell.c's new
   `&&`/`||` chaining (execute()) always sees a nonzero, "this failed"
   status for them, exactly like a real spawned process's own nonzero
   exit code would. */
#define shellExecNotFound 127

/* The shell used to own the only "current directory" concept in this whole
   kernel, a single static local shared by nothing else -- sched.h's new
   Thread.cwd (per-thread, real chdir(2)/getcwd(2) semantics, vfs.h's
   vfsChdir/vfsGetcwd/vfsResolvePath) replaces it: the shell is a plain
   kernel thread like any other (see shellRun's own comment below), so it
   simply becomes one more caller of the same real subsystem instead of
   keeping a second, disconnected notion of "cwd" in parallel. This also
   means a job this shell spawns now genuinely inherits whatever directory
   the shell itself is currently in (initialCwd, sched.c) -- true before
   this fala only by accident of both defaulting to "/", never actually
   wired together. */

/*
 * Real job control for the interactive shell (jobctl.h does the actual
 * spawning/terminal-ownership work): any command line that is not one of
 * the built-ins above is tried as /bin/<word> instead of just being
 * rejected outright, with the rest of the line tokenized into a real argv
 * exactly like a normal shell does. A trailing lone "&" token backgrounds
 * it (jobctlRunBackground, no terminal, returns immediately) instead of
 * running it in the foreground (jobctlSpawnForeground + jobctlWaitForeground,
 * which blocks until it exits with the terminal genuinely handed to it --
 * see jobctl.c).
 */
#define maxBackgroundJobs 8

typedef struct BackgroundJob {
    uint32 pid;
    char name[32];
    bool used;
} BackgroundJob;

static BackgroundJob backgroundJobs[maxBackgroundJobs];
static uint32 lastSpawnedPid = threadInvalidId;

/* Real shell `$?` -- the exit status of the most recently completed
   command-or-pipeline-or-builtin, persisting across SEPARATELY typed
   lines (unlike the `&&`/`||` chaining `status` local in execute() below,
   which only ever lives for the duration of one typed line's own
   segments). Initialized to 0, matching a real shell's own `$?` before
   anything has ever run. Updated in exactly one place (execute(), right
   after every executeSingle() call, chained or not) and read in exactly
   one place (shellExpandToken, via shellIsStatusVarToken) -- the same
   "shell is a plain kernel thread, needs no syscall to reach its own
   state" shape doExport/doUnset/changeDir already use for their own
   thread-local state, just kept in a file-scope static here since $? is
   a shell-language special parameter, not a real environment variable
   (real shells never let you `export $?` either). */
static int64 lastStatus = 0;

uint32 shellLastSpawnedPid(void)
{
    return lastSpawnedPid;
}

/* Opportunistically reaps any of THIS shell's own children that have
   already exited (threadJoinAnyChild, sched.h -- the same non-blocking
   waitpid(-1)-shaped scan user/waitanytest.c already proves) and prints
   what finished. Only ever finds background jobs here: a foreground job is
   already fully joined-and-reaped by jobctlWaitForeground itself before
   that call ever returns, so by the time control comes back to this
   function there is nothing left over from it to double-reap. */
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
    /* First pass: count top-level '|' occurrences without mutating
       anything, so a too-many-stages line can be rejected with `line`
       completely untouched -- the same "validate before mutating" shape
       shellExtractRedirects already uses for its own dangling-operator
       check. */
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
    /* Same "count first, mutate nothing, reject up front" shape
       shellSplitPipelineStages uses for its own analogous limit --
       matching two-character operators one at a time (never overlapping:
       stepping by 2 on a match means "&&&" is one "&&" followed by a lone
       '&' left inside a segment, not two overlapping matches). */
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

    /* Validate BEFORE mutating anything: a dangling operator (no filename
       token after it) must leave argv and argc completely untouched,
       exactly like shellSplitPipelineStages leaves `line` untouched when it
       finds no pipe -- the caller needs to tell "malformed, nothing
       changed" apart from "changed, here is the result" without having to
       snapshot argv itself first. */
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

/* Shared expansion rule behind both shellExpandArgv (per-argv-position, an
   entire spawned program's argument list) and shellExpandRedirect
   (per-redirect-target) below: `$?` (shellIsStatusVarToken, this shell's
   own most-recently-completed status, lastStatus above) is checked FIRST
   since shellParseVarRef can never match it anyway (see
   shellIsStatusVarToken's own doc comment in shell.h), then a real
   `$NAME` environment lookup (shellParseVarRef + currentThreadGetEnv, an
   unset name expanding to an empty string like a real shell's own
   default). Anything that is neither is returned completely untouched,
   still pointing into whatever buffer the caller originally handed it --
   `storage` is only ever written to, and only ever the return value, when
   an expansion actually happened. */
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

/* Resolves every `$?`/`$NAME` token in argv[0..argc) via shellExpandToken
   above, overwriting argv[i] with a pointer into storage[i] whenever one
   expands -- necessary because argv[i] itself is a pointer into the
   shared line buffer with only enough room for the ORIGINAL (unexpanded)
   token, never enough for whatever length the variable's value happens to
   be. */
static void shellExpandArgv(char** argv, int argc, char storage[][envEntryMax])
{
    for (int i = 0; i < argc; i++)
        argv[i] = (char*)shellExpandToken(argv[i], storage[i], envEntryMax);
}

/* Expands `$?`/`$NAME` (shellExpandToken above) in each of ShellRedirect's
   own target fields (inPath/outPath/errPath/hereString) in place -- these
   come straight from shellExtractRedirects, which only ever pulls raw
   tokens out of argv with no expansion of its own, so a redirect target or
   here-string text written as `$NAME`/`$?` was previously left completely
   unexpanded (unlike every other argv position, which shellExpandArgv
   already covered) -- an honestly-scoped gap flagged when `$NAME`
   expansion was first added, closed here. Each field gets its own
   dedicated storage slot since all four could need to be expanded on the
   SAME command line at once (e.g. `cmd < $IN > $OUT 2> $ERR`). */
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

/* Storage capacity for one shellExpandGlobs match's resolved path -- a
   smaller, dedicated constant rather than reusing pathCapacity, the same
   "arbitrary but documented" fixed-size trade-off shellExpandArgv's own
   envEntryMax already makes for a $NAME value, since this buffer gets
   multiplied by shellMaxArgs (and, in runPipelineStages, by
   shellMaxPipelineStages again on top of that) -- see shellExpandGlobs'
   own doc comment for why that multiplication matters for a 16KiB kernel
   thread stack (sched.c's threadStackSize). */
#define shellGlobPathCapacity 64

/* Expands any argv[i] (i in [0,argc)) containing a glob metacharacter into
   the real directory entries matching it (VFS child-list order, vfsChild/
   vfsNext -- the same traversal listDir's own `ls` builtin above already
   uses), replacing that one token with however many real paths matched,
   written into `storage` (one shellGlobPathCapacity-byte slot per FINAL
   argv position, not per original token) and pointed at from `argv` in
   place. A token with no glob character is passed through completely
   unchanged. A token that DOES contain a glob character but matches NO
   real file is ALSO passed through completely unchanged, as its own
   literal single token (bash's own default nullglob=off behavior) -- so a
   program that tries to open a nonexistent glob gets the same "no such
   file" error a real shell gives for an unmatched pattern, not a silently
   vanished argument. Only the FINAL path component may contain glob syntax
   (shellSplitGlobToken's own doc comment) -- `/tmp/gd/ *.txt` is supported,
   `/tmp/ * /file.txt` is not. The directory part is resolved through
   vfsResolvePath first, so a bare `*.txt` (no '/' at all) globs the
   CALLING thread's own current directory, same cwd-awareness every other
   path-taking builtin in this file already gets. Deliberately called by
   runProgram/runPipelineStages AFTER shellExtractRedirects has already
   pulled out redirection operators and their targets, and after the
   trailing "&" background marker has already been stripped -- so a glob
   character can never appear in a redirect target or be mistaken for
   something other than a real program argument; an honestly-scoped
   limitation matching this file's existing "no stage may be a builtin"/
   redirect-target-scope documented choices elsewhere. Returns the new argc,
   or -1 (argv left completely untouched) if expansion would need more than
   `maxArgc` slots -- the same "malformed/too-large input, caller reports an
   error instead of silently truncating" shape shellSplitPipelineStages
   already uses for "too many stages". */
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

/* Fallback for a command that matched none of the built-ins in execute()
   below: `line` is already the first word (NUL-terminated in place by
   execute()'s own split) and `argument` is the rest of the original typed
   line, still starting right after that NUL in the very same buffer. */
/* Returns the real exit status of whatever this line ran (0/nonzero from
   the spawned process itself for a foreground command, shellExecNotFound
   above for anything that never even got to spawn, or 0 immediately for a
   backgrounded job -- matching real `cmd &`'s own immediate-success exit
   status, never the job's own eventual, still-unknown-at-this-point exit
   code) -- feeds shell.c's new `&&`/`||` chaining (execute()) the same way
   listDir/catFile/makeDir/changeDir/doExport/doUnset's own int returns
   already do for builtins. */
static int64 runProgram(char* line, char* argument)
{
    char* argv[shellMaxArgs];
    int argc = 0;
    argv[argc++] = line;
    argc += shellTokenize(argument, &argv[argc], shellMaxArgs - argc);

    /* argv[0] (== line, the program name) is deliberately left out of
       expansion: runProgram resolves the binary to spawn from `line`
       itself, never from argv[0], so expanding it would let argv[0] show
       the child a different name than the path actually loaded. */
    char expandedArgv[shellMaxArgs][envEntryMax];
    shellExpandArgv(argv + 1, argc - 1, expandedArgv + 1);

    bool background = (argc > 1 && strcmp(argv[argc - 1], "&") == 0);
    if (background)
        argc--;

    /* `< IN`/`<<< TEXT`/`> OUT`/`>> OUT`/`2> ERR`/`2>> ERR` are stripped out
       of argv here, AFTER the trailing "&" check above (so "&" itself is
       never mistaken for a redirection target) and BEFORE the program ever
       sees argv -- exactly like a real shell, which never hands its own
       redirection operators to the child as arguments. */
    ShellRedirect redirect;
    if (!shellExtractRedirects(argv, &argc, &redirect)) {
        kprintf("%s: redirection operator with no filename\n", line);
        return shellExecNotFound;
    }
    char redirectStorage[4][envEntryMax];
    shellExpandRedirect(&redirect, redirectStorage);

    /* Wildcard expansion runs LAST, on the real argv the child program will
       actually receive -- after redirects/here-string/background have all
       already been pulled out, so a glob character can only ever expand a
       genuine program argument (see shellExpandGlobs' own doc comment for
       why). argv[0] (the program name) is excluded, same reasoning as
       shellExpandArgv's own exclusion just above. */
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
        /* A here-string needs its own spawn path (jobctlRunBackgroundHereString
           wires the child's fd 0 to a real pipe the SHELL itself writes the
           text into, rather than opening a path) -- redirect.inPath is
           always nullptr whenever hereString is set (shellExtractRedirects'
           own last-one-wins rule between the two), so the plain Redirected
           call below would otherwise just leave fd 0 at the console. */
        bool spawned = redirect.hereString
            ? jobctlRunBackgroundHereString(binPath, (const char* const*)argv, argc,
                                             redirect.hereString, redirect.outPath,
                                             redirect.appendOut, redirect.errPath,
                                             redirect.appendErr, &pid)
            : jobctlRunBackgroundRedirected(binPath, (const char* const*)argv, argc,
                                             redirect.inPath, redirect.outPath, redirect.appendOut,
                                             redirect.errPath, redirect.appendErr, &pid);
        if (!spawned) {
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

    /* Split into spawn-then-wait (rather than one combined call) so
       lastSpawnedPid below is visible to anyone asking (shellLastSpawnedPid)
       the instant the job exists, not only after jobctlWaitForeground's
       blocking wait has already finished -- the same immediacy the
       background branch above already gets for free from jobctlRunBackground
       never blocking in the first place. Same here-string-vs-plain split as
       the background branch above, same reason. */
    uint32 pid = redirect.hereString
        ? jobctlSpawnForegroundHereString(binPath, (const char* const*)argv, argc,
                                           redirect.hereString, redirect.outPath,
                                           redirect.appendOut, redirect.errPath, redirect.appendErr)
        : jobctlSpawnForegroundRedirected(binPath, (const char* const*)argv, argc,
                                           redirect.inPath, redirect.outPath,
                                           redirect.appendOut, redirect.errPath,
                                           redirect.appendErr);
    if (pid == threadInvalidId) {
        kprintf("unknown command or bad redirection: %s (try 'help')\n", line);
        return shellExecNotFound;
    }
    lastSpawnedPid = pid;
    int64 exitCode = jobctlWaitForeground(pid);
    kprintf("%s exited with code %ld\n", line, exitCode);
    return exitCode;
}

/* Splits each of an already-`shellSplitPipelineStages`d line's stages into
   its own binary name + argv, exactly like runProgram's own split above --
   each stage is independently "the first word, then the rest" in the SAME
   shape execute() already hands runProgram, just done once per stage here
   instead of once total. */
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

/* Real N-stage shell pipeline (2 <= count <= shellMaxPipelineStages):
   stage i's stdout feeds stage i+1's stdin through a real pipe
   (vfsPipeCreateHandle + jobctlSpawnPiped's fd-baking, jobctl.h) for every
   adjacent pair -- stage 0 otherwise keeps the real console for its stdin,
   the last stage otherwise keeps it for its stdout, exactly like a real
   shell's own `cmd1 | cmd2 | ... | cmdN`. jobctlSpawnPiped already takes
   independent stdin/stdout pipe handles per call, so a MIDDLE stage (both
   ends piped at once) needs no new primitive here, only the loop below
   wiring each stage's stdin to the previous pipe and its stdout to the
   next one. No stage may itself be a builtin (echo/ls/cat/... run directly
   on THIS thread via kprintf/vfs, never through a spawned process's own fd
   1, so they have no real fd to redirect) -- an honestly-scoped limitation
   matching jobctl.h's own doc comments elsewhere in this file. Only stage
   0's stdin may additionally be a here-string (`cmd1 <<< TEXT | cmd2 | ...`,
   jobctlSpawnPipedHereString below) -- every later stage's stdin is already
   a pipe, exactly like the old two-stage runPipeline already required.
   Returns the real exit status of the whole pipeline (the LAST stage's own
   exit code, matching real `$?`, or shellExecNotFound for any error that
   never even got every stage spawned) -- same "feeds shell.c's new
   `&&`/`||` chaining" reasoning as runProgram's own return above. */
static int64 runPipelineStages(char* stages[], int count)
{
    char* word[shellMaxPipelineStages];
    char* argv[shellMaxPipelineStages][shellMaxArgs];
    int argc[shellMaxPipelineStages];
    ShellRedirect redirect[shellMaxPipelineStages];
    /* Per-stage expansion scratch for shellExpandArgv, same "argv[0] is
       never expanded" reasoning as runProgram's own expandedArgv -- each
       stage's actual binary comes from word[i], never from argv[i][0]. */
    char expanded[shellMaxPipelineStages][shellMaxArgs][envEntryMax];
    /* Per-stage wildcard expansion scratch (shellExpandGlobs) -- a
       dedicated, smaller shellGlobPathCapacity buffer rather than reusing
       pathCapacity, since this array is already multiplied by
       shellMaxPipelineStages on top of shellMaxArgs and shares a single
       16KiB kernel-thread stack (sched.c's threadStackSize) with every
       other local array in this function -- see shellExpandGlobs' own doc
       comment in shell.c for the full sizing reasoning. */
    char globStorage[shellMaxPipelineStages][shellMaxArgs][shellGlobPathCapacity];
    /* Per-stage redirect-target expansion scratch (shellExpandRedirect) --
       same reasoning as runProgram's own redirectStorage, one dedicated
       4-slot array per stage since a stage's own inPath/outPath/errPath/
       hereString could all need expanding on the same line at once. */
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

        /* Same "expand real arguments last, after redirects" ordering as
           runProgram's own single-command path above. */
        int expandedArgc = shellExpandGlobs(&argv[i][1], argc[i] - 1, shellMaxArgs - 1, globStorage[i]);
        if (expandedArgc < 0) {
            kprintf("%s: too many arguments after wildcard expansion\n", word[i]);
            return shellExecNotFound;
        }
        argc[i] = 1 + expandedArgc;
    }

    /* Only the two OPEN ends of the pipeline may be file/here-string
       redirected for stdin/stdout -- stage 0's stdin and the LAST stage's
       stdout -- exactly like this function's own header comment already
       scopes "no stage may be a builtin" for a similar reason. Every
       interior fd is already wired to a real pipe on both sides, so those
       combinations are rejected outright below rather than silently
       guessing which of "the pipe" or "the file" should win. fd 2
       (stderr) is never part of any pipe, so `2>`/`2>>` are always allowed
       on every stage independently, with no such restriction. */
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
    /* pipeHandle[i] connects stage i's stdout to stage i+1's stdin -- only
       count-1 links exist for count stages. */
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

        /* joinPgid: every stage after the first joins stage 0's own group
           (jobctlSpawnPiped's own doc comment) so a single Ctrl-C, or this
           function's own eventual keyboardSetForegroundPgid via
           jobctlWaitPipelineStages, reaches every stage at once. */
        uint32 joinPgid = (i == 0) ? threadInvalidId : pids[0];

        char binPath[binPathCapacity];
        snprintf(binPath, sizeof(binPath), "/bin/%s", word[i]);

        /* A here-string is only ever possible on stage 0 (validated above)
           -- jobctlSpawnPipedHereString wires fd 0 to a real pipe the
           SHELL itself writes the text into, rather than leaving it at the
           console, then passes stdoutHandle/joinPgid straight through
           exactly like the plain jobctlSpawnPiped every other stage
           uses. */
        uint32 pid = (i == 0 && redirect[i].hereString)
            ? jobctlSpawnPipedHereString(binPath, (const char* const*)argv[i], argc[i],
                                          redirect[i].hereString, stdoutHandle, joinPgid)
            : jobctlSpawnPiped(binPath, (const char* const*)argv[i], argc[i],
                                stdinHandle, stdoutHandle, joinPgid);

        if (pid == threadInvalidId) {
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

/* One line of `ls` output for a single entry, real stat data (fs.h's
   FbStat) already resolved by the caller -- shared by both the plain and
   `-l` formats and by the "argument names a file, not a directory" case
   below, so all three ever only format an FbStat one way. */
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

    /* Deliberately prints type/size/name BEFORE the date: a needle that
       greps for a known type+size+name substring (tools/ci-test.sh) then
       never has to also match today's wall-clock date, which is real and
       therefore different on every run. The real owner/group/other rwx
       permission string (fs.h's fsModeOwner*, fsModeGroup*, fsModeOther*) is
       appended AFTER the date for the same reason, in the other direction
       -- every existing needle that greps a type+size+name+date-prefix
       substring keeps matching unchanged, since nothing before it in the
       line moved (only perm itself grew longer, from 3 chars to the full
       9-char rwxrwxrwx real `ls -l` prints, now that a real gid exists to
       make the group triad meaningful). */
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
        /* tmpfs/dev entries never track time (fs.h's FbStat comment) --
           an honest "no timestamp" marker instead of a fake 1970 date. */
        kprintf("- %s\n", perm);
    } else {
        RtcDateTime dt;
        rtcCivilFromEpoch((int64)stat->mtime, &dt);
        kprintf("%04ld-%02ld-%02ld %02ld:%02ld:%02ld %s\n",
                dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second, perm);
    }
}

/* `ls [-l] [PATH]` -- generalized over vfsOpen/vfsGetdents/vfsStat
   (fs.h/vfs.h) instead of the old tmpfs-only vfsLookup/vfsChild/vfsNext
   VfsNode-tree walk, so a directory under `/mnt` (SFS) can finally be
   listed from the interactive shell at all (previously vfsLookup, which
   only ever understood the in-memory tmpfs tree, silently returned "no
   such path" for every SFS directory -- fstest.c/linktest.c could only
   ever prove SFS directory listing through a raw userGetdents syscall,
   never through this builtin). `-l` adds real stat data per entry (type,
   size, mtime -- sfs.h's on-disk timestamps, FALA 39) the same way a real
   `ls -l` would. */
/* Returns 0 on success, 1 on failure (real POSIX `ls` exit-status shape) --
   feeds shell.c's new `&&`/`||` conditional chaining (execute()) the same
   way a spawned program's own real exit code already does, see
   shellSplitAndOr's own doc comment in shell.h. */
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

/* Same "0 success, 1 failure" shape listDir above uses. */
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

/* Same "0 success, 1 failure" shape listDir/catFile above use. */
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

/* Real `rm`: refuses a directory outright (real rm without -r never
   descends), matching vfsUnlink's own EISDIR-shaped refusal (vfs.c) --
   the vfsStat check here exists only to give a friendlier message than
   vfsUnlink's plain -1 would on its own, the actual enforcement is
   vfsUnlink's, not this. */
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

/* Real `rmdir`: only ever removes an EMPTY directory, same split from `rm`
   above that real rmdir(2)/unlink(2) have. */
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

/* Real `chmod MODE PATH`: MODE is parsed as octal (strtoul base 8, klib.h),
   matching real chmod(1)'s numeric-mode convention -- "755" already means
   0755, no leading 0 required. Both the owner AND other triads are
   meaningful now that a real uid model exists (fs.h's fsModeOwner and
   fsModeOther bits, vfs.c's vfsPermitted) -- refused outright unless the
   caller owns the target or is uid 0 (vfsChmod's own vfsOwnerOrRoot check).
   Symbolic modes (u+x, a=r, ...) are not supported, the same "numeric only"
   scope every other new builtin in this shell has picked over a fuller
   grammar it does not need. */
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

/* Real `chown UID PATH`: UID is parsed as a plain decimal number (strtoul
   base 10, klib.h), unlike chmod's octal MODE -- a uid is never a bitmask,
   so there is no "leading digit means octal" convention to match. Refused
   outright unless the caller is uid 0 (vfsChown's own root-only check) --
   even the file's own owner may not give it away, matching real Unix's
   _POSIX_CHOWN_RESTRICTED default. */
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

/* Real `chgrp GID PATH`: decimal like chown, not octal like chmod -- a gid
   is never a bitmask either. Unlike chown (root-only, full stop), the
   file's own owner may also call this, but only to set the group to THIS
   shell's own current gid (vfsChgrp's own owner-may-only-name-their-own-gid
   rule, see its doc comment) -- root may still set any gid. */
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

/* Same "0 success, 1 failure" shape as above. */
static int changeDir(const char* argument)
{
    char path[pathCapacity];
    if (argument[0] == 0)
        memcpy(path, "/", 2);
    else
        buildPath(argument, path);

    /* vfsChdir (vfs.h) also understands "/mnt" directories -- unlike the
       vfsLookup+vfsIsDir check this used to do, which only ever walked the
       VfsNode tree (ustar/tmpfs) and so could never actually cd into the
       persistent on-disk filesystem at all. */
    if (vfsChdir(path) != 0) {
        kprintf("cd: no such directory: %s\n", path);
        return 1;
    }
    return 0;
}

/* `export NAME=VALUE` -- real POSIX shell syntax, no quoting support
   (matching every other built-in in this shell, e.g. doEcho below never
   understands quotes either): the FIRST '=' splits `argument` into name
   and value, everything after it becomes the value verbatim (including any
   embedded spaces), the same "rest of the line, taken as-is" shape doEcho
   already gives its own unredirected text. currentThreadSetEnv (sched.h)
   sets it directly in THIS thread's own table -- the shell is a plain
   kernel thread (shellRun's own comment above), so it needs no syscall to
   reach its own state, exactly like changeDir calling vfsChdir directly
   above. Any job spawned AFTER this call (jobctl.h, via threadCreateUser's
   initialEnv) inherits a real copy of the new value. */
/* Same "0 success, 1 failure" shape listDir/catFile/makeDir/changeDir
   above use. */
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

/* `unset NAME` -- real POSIX shell syntax, removes `argument` from this
   thread's own environment table via currentThreadUnsetEnv (sched.h),
   the exact same "no syscall needed, shell is a plain kernel thread"
   shape as doExport above. Silently succeeds whether or not `argument`
   actually had an entry (matching real unsetenv(3)'s own no-op-success
   semantics for an already-absent name) -- the usage error below is only
   for the "no argument at all" case, not "name not found", and (same "0
   success, 1 failure" shape listDir/catFile/makeDir/changeDir/doExport
   above use) the only way this can ever return failure. */
static int doUnset(char* argument)
{
    if (argument[0] == 0) {
        kprintf("unset: usage: unset NAME\n");
        return 1;
    }
    currentThreadUnsetEnv(argument);
    return 0;
}

/* `env` -- lists every "NAME=VALUE" this shell thread currently holds
   (currentThreadEnvCount/currentThreadEnvEntry, sched.h), the same raw
   storage format doExport above writes and shellExpandArgv's $NAME lookups
   read. */
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

/* Upper bound on how many `$(...)` command substitutions execute() will
   expand on one typed line -- the same "generous cap for one
   lineCapacity-bounded line, not a claim real shells have such a limit"
   reasoning shellMaxPipelineStages/shellMaxAndOrSegments already give
   their own per-line caps. */
#define shellMaxCommandSubst 4

/* Expands every non-nested `$(cmd [args])` in `srcLine` into `outLine`,
   left to right -- called from executeSingle below only once `&&`/`||`
   (execute()'s own shellSplitAndOr, over the ORIGINAL typed line) and `|`
   (executeSingle's own shellSplitPipelineStages, over THIS segment) have
   already been resolved, so a literal `&&`/`||`/`|` byte sitting inside a
   substituted command's own captured output can never be mistaken for a
   real operator -- those are only ever recognized once, before any
   expansion runs, exactly like a real shell's own grammar (operators are
   parsed structurally first; expansion happens per-word afterward, never
   the other way around).
   Builds the result into a PRIVATE `work` copy first, splicing in place
   there, rather than mutating `srcLine` itself: `srcLine` here is not
   necessarily its own standalone buffer -- shellSplitAndOr/
   shellSplitPipelineStages both split their input IN PLACE by writing NUL
   terminators into ONE shared backing array (shell.h's own doc comments on
   both), so a segment that still has ANOTHER segment's text sitting
   immediately after its own NUL terminator would have that neighbor
   silently corrupted by any in-place splice that grows past the original
   text's own length. Working in a fully separate `work`/`outLine` buffer
   makes that impossible regardless of how much the line grows or shrinks.
   Each `$(...)` found (shellFindCommandSubst, shell.h) is run to
   completion via jobctlCaptureOutput -- a REAL spawn, not a builtin (no fd
   of its own to capture from, the same "no stage may be a builtin" scope
   runPipelineStages already documents) -- with its own argv given
   `$NAME`/`$?` expansion (shellExpandArgv) but nothing else: no wildcard
   globs, no redirection, no pipes, no further `$(...)` nesting. Trailing
   newlines are stripped from the captured output, matching a real shell's
   own `$(...)`. Only ever scans PAST whatever it just spliced into `work`
   (`scanFrom` below), never re-scanning a command's own captured output
   for a further `$(` -- the same single-pass, no-second-expansion rule a
   real shell gives command substitution's own result.
   Returns true having written the fully expanded line into `outLine`
   (unchanged from `srcLine` if there was nothing to expand), or false,
   having already reported the error via kprintf and left `outLine`
   untouched, on a malformed `$(` with no matching `)`, an unknown command,
   a spliced result that would overflow lineCapacity, or more than
   shellMaxCommandSubst substitutions on one line. */
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
        /* Deliberately discarded: real shells leave `$?` reflecting the
           OUTER command that actually runs after substitution completes,
           never the substituted command itself (executeSingle/execute
           already set lastStatus from whatever this line goes on to
           spawn). */
        (void)status;
        if (!spawned) {
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

/* Runs exactly one command-or-pipeline (never itself another `&&`/`||`
   chain -- shellSplitAndOr has already peeled those apart before this is
   ever called, see execute() below) and returns its real exit status: a
   builtin's own 0/1 (listDir/catFile/makeDir/changeDir/doExport/doUnset),
   0 for every other builtin (none of them has a documented failure case),
   or whatever runProgram/runPipelineStages themselves return for a
   spawned command or pipeline. This used to be the entire body of
   execute() itself, before shell.c had any notion of chaining multiple
   commands' exit statuses together. */
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

    /* `$(cmd)` command substitution (shellExpandCommandSubst above) only
       ever runs for a single, non-piped command -- an honestly-scoped
       limitation matching this file's own history of landing a feature for
       the plain case first and generalizing to pipeline stages in a later
       fala (`<<<` here-strings took the same path: jobctlSpawnForegroundHereString
       landed before jobctlSpawnPipedHereString ever fed a pipeline's first
       stage). `|` has already been ruled out above (pipeStageCount == 0),
       so nothing here can still be misread as a pipe. */
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

/* Splits `line` at every top-level `&&`/`||` (shellSplitAndOr, shell.h)
   and runs each segment left to right through executeSingle above,
   short-circuiting exactly like a real shell: a segment preceded by `&&`
   only runs if the PREVIOUS segment that actually ran (not necessarily the
   lexically previous one -- a skipped segment leaves `status` completely
   unchanged) exited 0, one preceded by `||` only runs if that same status
   was nonzero. This is real conditional chaining WITHIN a single typed
   line (`cmd1 && cmd2 || cmd3`, the classic shell if-then-else idiom).
   `lastStatus` (this file's own file-scope static above) is updated right
   after every executeSingle() call below, chained or not -- a real shell's
   own `$?` updates after EVERY simple command, including mid-chain (so
   `false || echo $?` sees `false`'s own status while evaluating `echo`'s
   argv, not some later value), and survives PAST this function returning,
   so a later, completely separate typed line can still read it via
   shellExpandToken's own `$?` handling -- closing the "no persistent `$?`
   across separately typed lines" gap this file used to document here as a
   deliberate limitation. A SKIPPED segment leaves `lastStatus` untouched
   exactly like it already leaves the local chaining `status` untouched. */
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

    /* The shell is its own independent console reader (keyboard.h), same as
       every ring-3 process's fd 0 -- a keystroke keyboardHandleScancode
       decodes is broadcast to every open reader, this one included, so the
       shell keeps working unchanged even while other processes are also
       blocked reading fd 0 (see the consoleReaderA/consoleReaderB live boot
       demo in kmain.c). Plain kernel-thread context (not a syscall
       handler), so the blocking helper's real halt() loop is safe here,
       exactly like pipeReadBlocking's callers. */
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
