#ifndef FROSTBYTE_SHELL_H
#define FROSTBYTE_SHELL_H

#include "types.h"

void shellRun(void);

/* Mirrors elf.h's elfMaxArgs (kept as shell.c's own named copy rather than
   including elf.h just for one constant, the same "each caller of this
   pattern owns its own named copy" choice jobctl.c/kmain.c/syscall.c
   already made for their own shared "one slot's worth of stack" constant)
   -- an argv built with more tokens than this could never be handed to
   elfBuildInitialStack anyway. */
#define shellMaxArgs 16

/* Upper bound on how many `|`-separated stages shellSplitPipelineStages
   below (and shell.c's runPipelineStages) will ever build at once -- a
   generous cap for anything a human could plausibly type on one
   lineCapacity-bounded (128 bytes, shell.c) command line, not a claim that
   real shells have such a limit. Named analogously to shellMaxArgs (a
   per-command cap) but for the orthogonal "how many commands are chained
   together" axis. */
#define shellMaxPipelineStages 4

/* Splits `text` in place (each run of spaces becomes a NUL terminator,
   like strtok but reentrant -- no hidden static state) into at most
   maxTokens pointers into that same buffer, returning how many it found.
   Takes an explicit capacity rather than hardcoding shellMaxArgs
   internally so a caller that already consumed some of its own argv[]
   array (runProgram, shell.c: argv[0] is always the program name itself,
   not something shellTokenize ever produces) can correctly pass only the
   REMAINING room, not the full array size -- passing the full array size
   there would let this function write one past the end of it. Exposed
   here (rather than kept static in shell.c) so shellTest.c (ktest.h) can
   exercise its edge cases -- leading/trailing/repeated spaces, empty
   input, hitting the capacity -- as pure string manipulation with no VFS
   or scheduler involved, safe to run from KTEST's pre-scheduler
   context. */
int shellTokenize(char* text, char* tokens[], int maxTokens);

/* Splits `line` in place at every top-level '|' into up to `maxStages`
   NUL-terminated stages, written into `stages[0..result-1]` -- shell.c's
   `cmd1 [args] | cmd2 [args] | ... | cmdN [args]` pipeline syntax, now a
   REAL N-stage chain rather than only ever the first two commands (jobctl.h's
   jobctlSpawnPiped already took independent stdin/stdout pipe handles per
   spawn from the day it was written, so a middle stage with BOTH ends piped
   at once needed no new kernel primitive -- only this function and
   shell.c's runPipelineStages were ever the limiting factor).
   Whitespace directly touching each '|' is trimmed from both sides of it
   (so "a | b | c" and "a|b|c" produce the same stages) -- shellTokenize's
   own token splitter only ever strips LEADING spaces from whatever it is
   handed, never trailing ones, so every stage but the last has its trailing
   spaces trimmed here first.
   Returns 0 (leaving `line` and `stages` completely untouched) if there is
   no '|' anywhere in the line at all, so the caller (execute()) knows to
   fall back to running a single command exactly as before. Returns -1
   (also leaving everything untouched) if the line contains MORE than
   `maxStages` stages -- a real shell has no such limit, but this kernel's
   per-pipeline arrays (shell.c's runPipelineStages, jobctl.c's
   jobctlWaitPipelineStages) are fixed-size, so a caller must be able to
   tell "malformed count" apart from "no pipe found" to report the right
   error instead of silently truncating a longer chain. Otherwise returns
   the real stage count (2..maxStages). Exposed here (rather than kept
   static in shell.c) for the same pre-scheduler, no-VFS KTEST coverage
   shellTokenize already gets (shellTest.c). */
int shellSplitPipelineStages(char* line, char* stages[], int maxStages);

/* Upper bound on how many `&&`/`||`-separated segments shellSplitAndOr
   below (and shell.c's execute()) will ever build at once -- the exact
   same "generous cap for one lineCapacity-bounded command line, not a
   claim real shells have such a limit" reasoning shellMaxPipelineStages
   above already gives, on the orthogonal "how many conditionally-run
   commands are chained together" axis. */
#define shellMaxAndOrSegments 4

/* The operator immediately BEFORE segments[i+1] in shellSplitAndOr's own
   output (there is no operator before segments[0], so andOrOps[] below
   only ever has count-1 meaningful entries) -- real shell `&&`
   (short-circuit: run the next command only if the previous one
   succeeded, exit status 0) vs `||` (run the next command only if the
   previous one FAILED, exit status != 0). */
typedef enum {
    shellAndOrAnd = 1,
    shellAndOrOr = 2
} ShellAndOrOp;

/* Splits `line` in place at every top-level `&&`/`||` into up to
   `maxSegments` NUL-terminated segments, written into
   `segments[0..result-1]`, with `andOrOps[0..result-2]` recording which
   operator sat between each adjacent pair -- shell.c's new
   `cmd1 && cmd2 || cmd3 && ...` conditional-chaining syntax, run BEFORE
   pipeline splitting (`|` binds tighter than `&&`/`||`, exactly like a
   real shell: `a | b && c` means "(a | b) && c", so each of THIS
   function's own segments is independently handed to
   shellSplitPipelineStages afterward, never the other way around).
   Whitespace directly touching each operator is trimmed from both sides
   of it, the same "a&&b and a && b produce the same segments" convention
   shellSplitPipelineStages already established for `|`. A lone `&`
   (shell.c's own trailing background marker, checked much later after
   this function has already run) is never mistaken for half of `&&`,
   since this function only ever matches the exact two-character sequence
   -- a single `&` anywhere else in the line is simply left inside
   whichever segment it falls in, untouched.
   Returns 0 (leaving `line` and `segments` completely untouched) if there
   is no top-level `&&`/`||` anywhere in the line, so the caller knows to
   run it as a single command-or-pipeline exactly as before. Returns -1
   (also leaving everything untouched) if the line contains MORE than
   `maxSegments` segments, the same "malformed count, do not silently
   truncate" shape shellSplitPipelineStages already uses for its own
   analogous limit. Otherwise returns the real segment count
   (2..maxSegments). Exposed here (rather than kept static in shell.c) for
   the same pre-scheduler, no-VFS KTEST coverage shellSplitPipelineStages
   already gets (shellTest.c). */
int shellSplitAndOr(char* line, char* segments[], ShellAndOrOp andOrOps[], int maxSegments);

/* Result of shellExtractRedirects below -- nullptr inPath/outPath/errPath/
   hereString means "no such redirection was found, leave that fd at its
   normal default". */
typedef struct ShellRedirect {
    const char* inPath;
    const char* outPath;
    bool appendOut;
    const char* errPath;
    bool appendErr;
    /* `<<<` TEXT -- shell.c's new here-string syntax. Unlike inPath (a file
       to open), this is the literal next token's own text, fed to the
       child's fd 0 through a real anonymous pipe the SHELL itself writes
       into (jobctl.h's jobctlSpawnForegroundHereString/
       jobctlRunBackgroundHereString), never opened as a path. Mutually
       exclusive with inPath at the SAME fd 0 -- see shellExtractRedirects'
       own doc comment below for the last-one-wins rule between the two. */
    const char* hereString;
} ShellRedirect;

/* Scans an already-tokenized argv (shellTokenize's own output shape) for
   `<`, `<<<`, `>`, `>>`, `2>`, and `2>>` tokens -- shell.c's new
   `cmd [ARGS] [< IN | <<< TEXT] [> OUT | >> OUT] [2> ERR | 2>> ERR]`
   redirection syntax -- and removes each one together with the token
   immediately after it (a filename for every operator except `<<<`, whose
   own following token is the literal here-string text instead), compacting
   the remaining tokens down to a real argv with no gaps (exactly what
   jobctl.h's spawn functions need to hand a program, which must never see
   its own redirection operators as if they were its own arguments).
   Writes the compacted count back through `argc` and fills `out` with
   whichever paths/text were found (nullptr for whichever operator never
   appeared). Returns false, leaving `argv`/`*argc` completely untouched, if
   `<`, `<<<`, `>`, `>>`, `2>`, or `2>>` appears as the LAST token with no
   filename/text after it -- the same "malformed input, caller falls back to
   reporting an error" shape shellSplitPipelineStages uses for "no pipe
   found" (though this one look more like an error since every OTHER token
   still gets scanned first, unlike shellSplitPipelineStages's single
   up-front counting pass). A later `>`/`>>` overwrites an earlier one (and vice versa)
   rather than erroring, the same "last one wins" a real shell gives
   `cmd > a > b` -- only ever ONE is meaningful since a single fd 1 can only
   be redirected once; `2>`/`2>>` follow the exact same last-one-wins rule
   independently for fd 2, since the two operator families target different
   fds and never conflict with each other. `<` and `<<<` follow the SAME
   last-one-wins rule for fd 0 across the two of them together (whichever
   appears later wins, clearing the other field) -- a real shell gives fd 0
   to whichever of `< file`/`<<< text` was written last too, since both
   target the exact same single fd. Exposed here (rather than kept static in
   shell.c) for the same pre-scheduler, no-VFS KTEST coverage
   shellTokenize/shellSplitPipelineStages already get (shellTest.c). */
bool shellExtractRedirects(char** argv, int* argc, ShellRedirect* out);

/* Recognizes shell.c's `$NAME` unquoted-parameter-expansion syntax (the
   glue that actually resolves it against a real environment, shell.c's
   new shellExpandArgv, is not pure -- it reads sched.h's Thread.env via
   currentThreadGetEnv, off limits for KTEST's pre-scheduler context -- so
   only the SYNTAX half lives here, the same "pure string logic here, the
   thread/VFS-touching glue stays static in shell.c" split shellTokenize/
   shellSplitPipelineStages/shellExtractRedirects above already use).
   `token` must be exactly '$' followed by one or more characters drawn
   from [A-Za-z0-9_] (real shell variable-name syntax) and nothing else;
   on a match, copies just the name (without the leading '$') into
   `nameOut` and returns true. Returns false, `nameOut` untouched, for a
   bare "$", a name that does not fit `nameCap`, or anything not starting
   with '$' at all -- callers treat "false" as "not a variable reference,
   leave this token exactly as typed". */
bool shellParseVarRef(const char* token, char* nameOut, int nameCap);

/* Recognizes shell.c's `$?` special parameter -- real POSIX shells' own
   most-recently-completed command status, distinct from any `$NAME`
   environment variable (shellParseVarRef above deliberately rejects it:
   '?' is not in its own [A-Za-z0-9_] name-character set, so the two
   token classes can never collide). `token` must be EXACTLY the two
   characters "$?" and nothing else -- same "whole token or nothing"
   contract shellParseVarRef already gives `$NAME`, so `$?x` or `x$?` are
   both rejected, left for the caller to treat as an ordinary literal
   token. The glue that resolves a match into an actual decimal status
   string (shell.c's shellExpandToken, which reads shell.c's own
   file-scope `lastStatus`) is not pure, so only this syntax check lives
   here -- the same "pure logic here, thread/state-touching glue stays
   static in shell.c" split shellParseVarRef already established. */
bool shellIsStatusVarToken(const char* token);

/* Classic shell wildcard matcher: '*' matches any run of characters
   (including none), '?' matches exactly one character, anything else must
   match `name` literally. Pure string logic (no VFS/scheduler involved),
   iterative rather than recursive (a single backtrack point remembering
   the most recent '*' and where in `name` it started matching from) --
   the same shape a real libc fnmatch(3) uses, and the same "pure logic
   here, thread/VFS-touching glue stays static in shell.c" split
   shellTokenize/shellSplitPipelineStages/shellExtractRedirects/
   shellParseVarRef above already established. Exposed here for
   shellTest.c's KTEST coverage. */
bool shellGlobMatch(const char* pattern, const char* name);

/* True if `token` contains at least one glob metacharacter ('*' or '?') --
   the same "is this token special" gate shellParseVarRef gives '$', used by
   shell.c's shellExpandGlobs to decide whether a token needs real directory
   listing + matching at all, or can pass through completely untouched (the
   common case for most argv words, which have no wildcard in them at
   all). */
bool shellTokenHasGlob(const char* token);

/* Splits `token` at its LAST '/' into `dirOut` (everything up to and
   including that '/', or an empty string if `token` has no '/' at all --
   meaning "the current directory", exactly like a bare `*.txt` in a real
   shell) and `patternOut` (everything after it, the actual glob pattern to
   match each directory entry's own name against via shellGlobMatch above).
   Only this FINAL path component may itself contain glob syntax -- `dirOut`
   is never re-scanned for `*`/`?` by anything in shell.c, an
   honestly-scoped limitation (e.g. `/tmp/gd/ *.txt` is supported,
   `/tmp/ * /file.txt` is not) matching this file's existing pattern of
   documenting what its syntax does NOT attempt (see runPipelineStages' own
   "no stage may be a builtin" and shellExtractRedirects' own redirect-target
   scope limits). Truncates silently (like every other fixed-capacity buffer
   in this shell) if either half does not fit its own capacity. Exposed here
   for the same pre-scheduler, no-VFS KTEST coverage every other pure
   function in this file already gets (shellTest.c). */
void shellSplitGlobToken(const char* token, char* dirOut, int dirCap, char* patternOut, int patternCap);

/* Recognizes shell.c's `$(cmd [args])` command-substitution syntax --
   locates the FIRST `$(` anywhere in `line` and its matching `)` (no
   nesting: the very next `)` after the opening `$(`, an honestly-scoped
   limitation matching shellSplitGlobToken's own "only the final path
   component" scope note), and copies the text between them (the command
   to run, still completely unparsed) into `cmdOut`, truncated to `cmdCap`
   like every other fixed-capacity buffer in this file.
   Returns true with `*startOut` set to -1 (cmdOut/`*endOut` left
   untouched) if `line` contains no `$(` at all -- the caller's signal to
   stop looking, exactly like shellSplitPipelineStages' own "0 means
   nothing found" convention but spelled with a sentinel index instead of a
   sentinel count, since a real match also needs to report WHERE in the
   line it sits. On a real match, `*startOut`/`*endOut` are the byte
   offsets of the leading `$` and one-past the matching `)` respectively,
   so a caller can splice `line[0..*startOut)` + (whatever `cmdOut`
   evaluates to) + `line[*endOut..]` into a new line the same way
   shellExtractRedirects' caller already reassembles argv around removed
   tokens. Returns false, leaving everything untouched, if a `$(` is found
   with no matching `)` anywhere after it -- the same "malformed input,
   caller reports an error" shape shellExtractRedirects already uses for an
   operator with no filename. Exposed here (rather than kept static in
   shell.c) for the same pre-scheduler, no-VFS KTEST coverage every other
   pure syntax function in this file already gets (shellTest.c) -- the glue
   that actually RUNS `cmdOut` and splices its captured output back in
   (shell.c's shellExpandCommandSubst) is not pure, so only the syntax half
   lives here, the same split shellParseVarRef/shellIsStatusVarToken above
   already established for `$NAME`/`$?`. */
bool shellFindCommandSubst(const char* line, int* startOut, int* endOut, char* cmdOut, int cmdCap);

/* The pid most recently spawned by the shell's own `NAME [ARGS] [&]`
   fallback (shell.c's runProgram) -- foreground or background alike, set
   the instant jobctlSpawnForeground/jobctlRunBackground (jobctl.h) hand
   one back, never only after a foreground job has already exited -- or
   threadInvalidId if the shell has never run anything yet. Lets a
   boot-time live demo (kmain.c) that drives the real interactive shell
   through injected keystrokes (there being no automated way to drive a
   real PS/2 keyboard under headless QEMU) find out what pid it just asked
   the shell to run, so it can act on that specific job (e.g. proving a
   backgrounded one stays alive across an injected Ctrl-C the same way
   user/pgroupbg.c's boot-time-spawned counterpart already does). */
uint32 shellLastSpawnedPid(void);

#endif
