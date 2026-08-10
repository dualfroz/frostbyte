#include "ktest.h"
#include "shell.h"
#include "string.h"

/* shellTokenize (shell.c) is the pure-string half of the shell's new
   `NAME [ARGS] [&]` job-control dispatch (runProgram, shell.c) -- turning
   a typed line into a real argv[] the way a normal shell does, instead of
   the single "rest of the line" string every other built-in command is
   content with. Everything below runs before schedStart() (ktest.h's own
   file comment), so it deliberately never touches the VFS, jobctl.h, or a
   real spawn -- only the tokenizer itself, which is ordinary deterministic
   string manipulation. */

KTEST(shellTokenizeEmptyLine)
{
    char text[] = "";
    char* tokens[4];
    int count = shellTokenize(text, tokens, 4);
    KT_EQ(t, count, 0);
}

KTEST(shellTokenizeSingleWord)
{
    char text[] = "hello";
    char* tokens[4];
    int count = shellTokenize(text, tokens, 4);
    KT_EQ(t, count, 1);
    KT_STREQ(t, tokens[0], "hello");
}

KTEST(shellTokenizeMultipleWordsCollapsesRepeatedSpaces)
{
    char text[] = "  one   two three  ";
    char* tokens[8];
    int count = shellTokenize(text, tokens, 8);
    KT_EQ(t, count, 3);
    KT_STREQ(t, tokens[0], "one");
    KT_STREQ(t, tokens[1], "two");
    KT_STREQ(t, tokens[2], "three");
}

KTEST(shellTokenizeAllSpacesYieldsNothing)
{
    char text[] = "     ";
    char* tokens[4];
    int count = shellTokenize(text, tokens, 4);
    KT_EQ(t, count, 0);
}

KTEST(shellTokenizeRespectsCapacityInsteadOfOverrunningArray)
{
    char text[] = "a b c d e";
    char* tokens[3];
    int count = shellTokenize(text, tokens, 3);
    KT_EQ(t, count, 3);
    KT_STREQ(t, tokens[0], "a");
    KT_STREQ(t, tokens[1], "b");
    KT_STREQ(t, tokens[2], "c");
}

KTEST(shellTokenizeTrailingAmpersandIsJustAnotherToken)
{
    /* runProgram (shell.c) is the one that gives "&" its background
       meaning by inspecting the LAST token itself -- shellTokenize's own
       job is only to split words apart, so it must hand "&" back
       unchanged like any other token, not special-case it. */
    char text[] = "sleepjob arg1 &";
    char* tokens[8];
    int count = shellTokenize(text, tokens, 8);
    KT_EQ(t, count, 3);
    KT_STREQ(t, tokens[0], "sleepjob");
    KT_STREQ(t, tokens[1], "arg1");
    KT_STREQ(t, tokens[2], "&");
}

KTEST(shellTokenizeZeroCapacityYieldsNothing)
{
    char text[] = "one two";
    char* tokens[1];
    int count = shellTokenize(text, tokens, 0);
    KT_EQ(t, count, 0);
}

/* shellSplitPipelineStages (shell.c) is the pure-string half of the shell's
   `cmd1 | cmd2 | ... | cmdN` pipeline syntax -- deciding whether execute()
   should run runPipelineStages (N spawned processes joined by N-1 real
   pipes) or fall back to a single command, exactly the same "pure logic, no
   VFS/scheduler" split shellTokenize already has from its own doc comment
   above. */

KTEST(shellSplitPipelineStagesNoPipeReturnsZero)
{
    char text[] = "ls -l";
    char* stages[shellMaxPipelineStages];
    int count = shellSplitPipelineStages(text, stages, shellMaxPipelineStages);
    KT_EQ(t, count, 0);
}

KTEST(shellSplitPipelineStagesBasicTwoStageSplit)
{
    char text[] = "producer|consumer";
    char* stages[shellMaxPipelineStages];
    int count = shellSplitPipelineStages(text, stages, shellMaxPipelineStages);
    KT_EQ(t, count, 2);
    KT_STREQ(t, stages[0], "producer");
    KT_STREQ(t, stages[1], "consumer");
}

KTEST(shellSplitPipelineStagesTrimsSpacesAroundEachPipe)
{
    char text[] = "pipesrc arg1  |  pipecat  |  pipesink arg2";
    char* stages[shellMaxPipelineStages];
    int count = shellSplitPipelineStages(text, stages, shellMaxPipelineStages);
    KT_EQ(t, count, 3);
    KT_STREQ(t, stages[0], "pipesrc arg1");
    KT_STREQ(t, stages[1], "pipecat");
    KT_STREQ(t, stages[2], "pipesink arg2");
}

KTEST(shellSplitPipelineStagesThreeStageSplitIsRealNotJustFirstPipe)
{
    /* Unlike the old two-way shellSplitPipeline this generalizes, every
       top-level '|' is now recognized -- a real N-stage pipeline, not just
       the first split with the rest left embedded in one text blob. */
    char text[] = "a | b | c";
    char* stages[shellMaxPipelineStages];
    int count = shellSplitPipelineStages(text, stages, shellMaxPipelineStages);
    KT_EQ(t, count, 3);
    KT_STREQ(t, stages[0], "a");
    KT_STREQ(t, stages[1], "b");
    KT_STREQ(t, stages[2], "c");
}

KTEST(shellSplitPipelineStagesEmptyLastStageYieldsEmptyString)
{
    char text[] = "producer |";
    char* stages[shellMaxPipelineStages];
    int count = shellSplitPipelineStages(text, stages, shellMaxPipelineStages);
    KT_EQ(t, count, 2);
    KT_STREQ(t, stages[0], "producer");
    KT_STREQ(t, stages[1], "");
}

KTEST(shellSplitPipelineStagesTooManyStagesReturnsNegativeOneWithoutMutating)
{
    char text[] = "a | b | c | d | e";
    char* stages[shellMaxPipelineStages];
    /* shellMaxPipelineStages is 4, but this line has 5 stages -- must be
       rejected outright, leaving `text` completely untouched, exactly the
       same "malformed, nothing changed" contract shellExtractRedirects'
       own dangling-operator case already uses. */
    int count = shellSplitPipelineStages(text, stages, shellMaxPipelineStages);
    KT_EQ(t, count, -1);
    KT_STREQ(t, text, "a | b | c | d | e");
}

/* shellSplitAndOr (shell.c) is the pure-string half of the shell's new
   `cmd1 && cmd2 || cmd3 ...` conditional-chaining syntax -- deciding
   whether execute() should short-circuit through several commands' real
   exit statuses (executeSingle, shell.c) or fall back to a single
   command-or-pipeline exactly as before, run BEFORE pipeline splitting
   (`|` binds tighter than `&&`/`||`), the same "pure logic, no
   VFS/scheduler" split every other parser above already has. */

KTEST(shellSplitAndOrNoOperatorReturnsZero)
{
    char text[] = "ls -l";
    char* segments[shellMaxAndOrSegments];
    ShellAndOrOp ops[shellMaxAndOrSegments - 1];
    int count = shellSplitAndOr(text, segments, ops, shellMaxAndOrSegments);
    KT_EQ(t, count, 0);
}

KTEST(shellSplitAndOrBasicAndSplit)
{
    char text[] = "mkdir /tmp/a && echo ok";
    char* segments[shellMaxAndOrSegments];
    ShellAndOrOp ops[shellMaxAndOrSegments - 1];
    int count = shellSplitAndOr(text, segments, ops, shellMaxAndOrSegments);
    KT_EQ(t, count, 2);
    KT_STREQ(t, segments[0], "mkdir /tmp/a");
    KT_STREQ(t, segments[1], "echo ok");
    KT_EQ(t, ops[0], shellAndOrAnd);
}

KTEST(shellSplitAndOrBasicOrSplit)
{
    char text[] = "false || echo fallback";
    char* segments[shellMaxAndOrSegments];
    ShellAndOrOp ops[shellMaxAndOrSegments - 1];
    int count = shellSplitAndOr(text, segments, ops, shellMaxAndOrSegments);
    KT_EQ(t, count, 2);
    KT_STREQ(t, segments[0], "false");
    KT_STREQ(t, segments[1], "echo fallback");
    KT_EQ(t, ops[0], shellAndOrOr);
}

KTEST(shellSplitAndOrMixedChainRecordsEachOperatorIndependently)
{
    /* The classic shell if-then-else idiom: "run b only if a succeeded,
       run c only if the segment that actually ran last (a or b) did NOT
       succeed" -- proves && and || are tracked as two independently
       recorded operators, not collapsed into one boolean. */
    char text[] = "a && b || c";
    char* segments[shellMaxAndOrSegments];
    ShellAndOrOp ops[shellMaxAndOrSegments - 1];
    int count = shellSplitAndOr(text, segments, ops, shellMaxAndOrSegments);
    KT_EQ(t, count, 3);
    KT_STREQ(t, segments[0], "a");
    KT_STREQ(t, segments[1], "b");
    KT_STREQ(t, segments[2], "c");
    KT_EQ(t, ops[0], shellAndOrAnd);
    KT_EQ(t, ops[1], shellAndOrOr);
}

KTEST(shellSplitAndOrTrimsSpacesAroundEachOperator)
{
    char text[] = "a&&b";
    char* segments[shellMaxAndOrSegments];
    ShellAndOrOp ops[shellMaxAndOrSegments - 1];
    int count = shellSplitAndOr(text, segments, ops, shellMaxAndOrSegments);
    KT_EQ(t, count, 2);
    KT_STREQ(t, segments[0], "a");
    KT_STREQ(t, segments[1], "b");
}

KTEST(shellSplitAndOrLeavesSingleTopLevelPipeInsideItsOwnSegment)
{
    /* '|' binds tighter than '&&'/'||': a lone pipe character must never be
       mistaken for half of '||' (which only matches two CONSECUTIVE '|'
       characters), so "a | b" stays a single, whole segment here -- only
       execute()'s own later shellSplitPipelineStages call on that segment
       ever splits it further. */
    char text[] = "a | b && c";
    char* segments[shellMaxAndOrSegments];
    ShellAndOrOp ops[shellMaxAndOrSegments - 1];
    int count = shellSplitAndOr(text, segments, ops, shellMaxAndOrSegments);
    KT_EQ(t, count, 2);
    KT_STREQ(t, segments[0], "a | b");
    KT_STREQ(t, segments[1], "c");
    KT_EQ(t, ops[0], shellAndOrAnd);
}

KTEST(shellSplitAndOrTooManySegmentsReturnsNegativeOneWithoutMutating)
{
    char text[] = "a && b && c && d && e";
    char* segments[shellMaxAndOrSegments];
    ShellAndOrOp ops[shellMaxAndOrSegments - 1];
    /* shellMaxAndOrSegments is 4, but this line has 5 segments -- must be
       rejected outright, leaving `text` completely untouched, the same
       "malformed, nothing changed" contract shellSplitPipelineStages' own
       analogous limit already uses. */
    int count = shellSplitAndOr(text, segments, ops, shellMaxAndOrSegments);
    KT_EQ(t, count, -1);
    KT_STREQ(t, text, "a && b && c && d && e");
}

/* shellExtractRedirects (shell.c) is the pure-token half of shell.c's new
   `cmd [ARGS] [< IN] [> OUT | >> OUT]` file redirection -- exactly the same
   "pure logic, no VFS/scheduler" split shellTokenize/shellSplitPipelineStages
   already have from their own doc comments above, operating on
   shellTokenize's own output shape (an already-split argv) instead of a
   raw line. */

KTEST(shellExtractRedirectsNoOperatorsLeavesArgvUnchanged)
{
    char text[] = "cat file.txt";
    char* argv[8];
    int argc = shellTokenize(text, argv, 8);
    ShellRedirect redirect;
    bool ok = shellExtractRedirects(argv, &argc, &redirect);
    KT_TRUE(t, ok);
    KT_EQ(t, argc, 2);
    KT_STREQ(t, argv[0], "cat");
    KT_STREQ(t, argv[1], "file.txt");
    KT_NULL(t, redirect.inPath);
    KT_NULL(t, redirect.outPath);
}

KTEST(shellExtractRedirectsOutputTruncate)
{
    char text[] = "sort > out.txt";
    char* argv[8];
    int argc = shellTokenize(text, argv, 8);
    ShellRedirect redirect;
    bool ok = shellExtractRedirects(argv, &argc, &redirect);
    KT_TRUE(t, ok);
    KT_EQ(t, argc, 1);
    KT_STREQ(t, argv[0], "sort");
    KT_NULL(t, redirect.inPath);
    KT_NOTNULL(t, redirect.outPath);
    KT_STREQ(t, redirect.outPath, "out.txt");
    KT_FALSE(t, redirect.appendOut);
}

KTEST(shellExtractRedirectsOutputAppend)
{
    char text[] = "sort >> out.txt";
    char* argv[8];
    int argc = shellTokenize(text, argv, 8);
    ShellRedirect redirect;
    bool ok = shellExtractRedirects(argv, &argc, &redirect);
    KT_TRUE(t, ok);
    KT_EQ(t, argc, 1);
    KT_STREQ(t, argv[0], "sort");
    KT_STREQ(t, redirect.outPath, "out.txt");
    KT_TRUE(t, redirect.appendOut);
}

KTEST(shellExtractRedirectsInputAndOutputTogetherKeepRealArgs)
{
    char text[] = "sort -r < in.txt > out.txt";
    char* argv[8];
    int argc = shellTokenize(text, argv, 8);
    ShellRedirect redirect;
    bool ok = shellExtractRedirects(argv, &argc, &redirect);
    KT_TRUE(t, ok);
    KT_EQ(t, argc, 2);
    KT_STREQ(t, argv[0], "sort");
    KT_STREQ(t, argv[1], "-r");
    KT_STREQ(t, redirect.inPath, "in.txt");
    KT_STREQ(t, redirect.outPath, "out.txt");
    KT_FALSE(t, redirect.appendOut);
}

KTEST(shellExtractRedirectsOperatorsCanComeBeforeRealArgs)
{
    /* A real shell accepts redirection operators anywhere on the line, not
       only trailing -- "> out.txt cat file.txt" is a weird but legal way
       to write "cat file.txt > out.txt". */
    char text[] = "> out.txt cat file.txt";
    char* argv[8];
    int argc = shellTokenize(text, argv, 8);
    ShellRedirect redirect;
    bool ok = shellExtractRedirects(argv, &argc, &redirect);
    KT_TRUE(t, ok);
    KT_EQ(t, argc, 2);
    KT_STREQ(t, argv[0], "cat");
    KT_STREQ(t, argv[1], "file.txt");
    KT_STREQ(t, redirect.outPath, "out.txt");
}

KTEST(shellExtractRedirectsLastOutputOperatorWins)
{
    char text[] = "cat > a.txt >> b.txt";
    char* argv[8];
    int argc = shellTokenize(text, argv, 8);
    ShellRedirect redirect;
    bool ok = shellExtractRedirects(argv, &argc, &redirect);
    KT_TRUE(t, ok);
    KT_EQ(t, argc, 1);
    KT_STREQ(t, argv[0], "cat");
    KT_STREQ(t, redirect.outPath, "b.txt");
    KT_TRUE(t, redirect.appendOut);
}

KTEST(shellExtractRedirectsDanglingOperatorFailsWithoutMutating)
{
    char text[] = "cat file.txt >";
    char* argv[8];
    int argc = shellTokenize(text, argv, 8);
    int originalArgc = argc;
    ShellRedirect redirect;
    bool ok = shellExtractRedirects(argv, &argc, &redirect);
    KT_FALSE(t, ok);
    KT_EQ(t, argc, originalArgc);
    KT_STREQ(t, argv[0], "cat");
    KT_STREQ(t, argv[1], "file.txt");
    KT_STREQ(t, argv[2], ">");
}

KTEST(shellExtractRedirectsNoTokensYieldsNoRedirects)
{
    char* argv[1];
    int argc = 0;
    ShellRedirect redirect;
    bool ok = shellExtractRedirects(argv, &argc, &redirect);
    KT_TRUE(t, ok);
    KT_EQ(t, argc, 0);
    KT_NULL(t, redirect.inPath);
    KT_NULL(t, redirect.outPath);
}

/* `2>`/`2>>` (stderr redirection) are a separate operator family from
   `<`/`>`/`>>` above, targeting fd 2 instead of fd 0/fd 1 -- covered on
   their own (truncate, append, combined with stdout, last-one-wins,
   dangling) plus one case proving the two families never interfere with
   each other even when both appear on the same line. */

KTEST(shellExtractRedirectsErrTruncate)
{
    char text[] = "prog 2> err.txt";
    char* argv[8];
    int argc = shellTokenize(text, argv, 8);
    ShellRedirect redirect;
    bool ok = shellExtractRedirects(argv, &argc, &redirect);
    KT_TRUE(t, ok);
    KT_EQ(t, argc, 1);
    KT_STREQ(t, argv[0], "prog");
    KT_NULL(t, redirect.inPath);
    KT_NULL(t, redirect.outPath);
    KT_NOTNULL(t, redirect.errPath);
    KT_STREQ(t, redirect.errPath, "err.txt");
    KT_FALSE(t, redirect.appendErr);
}

KTEST(shellExtractRedirectsErrAppend)
{
    char text[] = "prog 2>> err.txt";
    char* argv[8];
    int argc = shellTokenize(text, argv, 8);
    ShellRedirect redirect;
    bool ok = shellExtractRedirects(argv, &argc, &redirect);
    KT_TRUE(t, ok);
    KT_EQ(t, argc, 1);
    KT_STREQ(t, argv[0], "prog");
    KT_STREQ(t, redirect.errPath, "err.txt");
    KT_TRUE(t, redirect.appendErr);
}

KTEST(shellExtractRedirectsStdoutAndStderrTogetherKeepRealArgs)
{
    char text[] = "sort -r > out.txt 2> err.txt";
    char* argv[8];
    int argc = shellTokenize(text, argv, 8);
    ShellRedirect redirect;
    bool ok = shellExtractRedirects(argv, &argc, &redirect);
    KT_TRUE(t, ok);
    KT_EQ(t, argc, 2);
    KT_STREQ(t, argv[0], "sort");
    KT_STREQ(t, argv[1], "-r");
    KT_STREQ(t, redirect.outPath, "out.txt");
    KT_FALSE(t, redirect.appendOut);
    KT_STREQ(t, redirect.errPath, "err.txt");
    KT_FALSE(t, redirect.appendErr);
}

KTEST(shellExtractRedirectsLastErrOperatorWins)
{
    char text[] = "cat 2> a.txt 2>> b.txt";
    char* argv[8];
    int argc = shellTokenize(text, argv, 8);
    ShellRedirect redirect;
    bool ok = shellExtractRedirects(argv, &argc, &redirect);
    KT_TRUE(t, ok);
    KT_EQ(t, argc, 1);
    KT_STREQ(t, argv[0], "cat");
    KT_STREQ(t, redirect.errPath, "b.txt");
    KT_TRUE(t, redirect.appendErr);
}

KTEST(shellExtractRedirectsDanglingErrOperatorFailsWithoutMutating)
{
    char text[] = "cat file.txt 2>";
    char* argv[8];
    int argc = shellTokenize(text, argv, 8);
    int originalArgc = argc;
    ShellRedirect redirect;
    bool ok = shellExtractRedirects(argv, &argc, &redirect);
    KT_FALSE(t, ok);
    KT_EQ(t, argc, originalArgc);
    KT_STREQ(t, argv[0], "cat");
    KT_STREQ(t, argv[1], "file.txt");
    KT_STREQ(t, argv[2], "2>");
}

/* `<<<` (here-string) is a third, independent fd-0 operator alongside `<` --
   covered on its own (basic recognition, combined with real args and `>`,
   dangling), plus the two last-one-wins cases proving `<` and `<<<` share
   ownership of the SAME fd 0 (whichever comes later in the line wins,
   clearing the other field entirely) rather than being two unrelated
   fields a caller could accidentally read both of at once. */

KTEST(shellExtractRedirectsHereStringBasic)
{
    char text[] = "cat <<< hello";
    char* argv[8];
    int argc = shellTokenize(text, argv, 8);
    ShellRedirect redirect;
    bool ok = shellExtractRedirects(argv, &argc, &redirect);
    KT_TRUE(t, ok);
    KT_EQ(t, argc, 1);
    KT_STREQ(t, argv[0], "cat");
    KT_STREQ(t, redirect.hereString, "hello");
    KT_NULL(t, redirect.inPath);
}

KTEST(shellExtractRedirectsHereStringWithRealArgsAndOutput)
{
    char text[] = "sort -r <<< hello > out.txt";
    char* argv[8];
    int argc = shellTokenize(text, argv, 8);
    ShellRedirect redirect;
    bool ok = shellExtractRedirects(argv, &argc, &redirect);
    KT_TRUE(t, ok);
    KT_EQ(t, argc, 2);
    KT_STREQ(t, argv[0], "sort");
    KT_STREQ(t, argv[1], "-r");
    KT_STREQ(t, redirect.hereString, "hello");
    KT_STREQ(t, redirect.outPath, "out.txt");
    KT_NULL(t, redirect.inPath);
}

KTEST(shellExtractRedirectsDanglingHereStringFailsWithoutMutating)
{
    char text[] = "cat <<<";
    char* argv[8];
    int argc = shellTokenize(text, argv, 8);
    int originalArgc = argc;
    ShellRedirect redirect;
    bool ok = shellExtractRedirects(argv, &argc, &redirect);
    KT_FALSE(t, ok);
    KT_EQ(t, argc, originalArgc);
    KT_STREQ(t, argv[0], "cat");
    KT_STREQ(t, argv[1], "<<<");
}

KTEST(shellExtractRedirectsHereStringAfterFileRedirectWins)
{
    char text[] = "cat < a.txt <<< b";
    char* argv[8];
    int argc = shellTokenize(text, argv, 8);
    ShellRedirect redirect;
    bool ok = shellExtractRedirects(argv, &argc, &redirect);
    KT_TRUE(t, ok);
    KT_EQ(t, argc, 1);
    KT_STREQ(t, redirect.hereString, "b");
    KT_NULL(t, redirect.inPath);
}

KTEST(shellExtractRedirectsFileRedirectAfterHereStringWins)
{
    char text[] = "cat <<< b < a.txt";
    char* argv[8];
    int argc = shellTokenize(text, argv, 8);
    ShellRedirect redirect;
    bool ok = shellExtractRedirects(argv, &argc, &redirect);
    KT_TRUE(t, ok);
    KT_EQ(t, argc, 1);
    KT_STREQ(t, redirect.inPath, "a.txt");
    KT_NULL(t, redirect.hereString);
}

/* shellParseVarRef (shell.c) is the pure-syntax half of the shell's new
   `$NAME` unquoted parameter expansion -- recognizing the "$" + name-chars
   shape, never touching sched.h's Thread.env (that lookup is
   shellExpandArgv's job, not KTEST-able here for the same pre-scheduler
   reason shellTokenize's own file comment gives). */
KTEST(shellParseVarRefAcceptsPlainName)
{
    char name[16];
    bool ok = shellParseVarRef("$HOME", name, sizeof(name));
    KT_TRUE(t, ok);
    KT_STREQ(t, name, "HOME");
}

KTEST(shellParseVarRefAcceptsDigitsAndUnderscore)
{
    char name[16];
    bool ok = shellParseVarRef("$_var_2", name, sizeof(name));
    KT_TRUE(t, ok);
    KT_STREQ(t, name, "_var_2");
}

KTEST(shellParseVarRefRejectsBareDollarSign)
{
    char name[16];
    KT_FALSE(t, shellParseVarRef("$", name, sizeof(name)));
}

KTEST(shellParseVarRefRejectsTokenWithoutLeadingDollar)
{
    char name[16];
    KT_FALSE(t, shellParseVarRef("HOME", name, sizeof(name)));
}

KTEST(shellParseVarRefRejectsEmbeddedPunctuation)
{
    char name[16];
    KT_FALSE(t, shellParseVarRef("$FOO=bar", name, sizeof(name)));
    KT_FALSE(t, shellParseVarRef("$FOO/bar", name, sizeof(name)));
}

KTEST(shellParseVarRefRejectsNameTooLongForCapacity)
{
    char name[4];
    KT_FALSE(t, shellParseVarRef("$TOOLONG", name, sizeof(name)));
}

/* shellParseVarRef must never accept `$?` itself -- '?' is not in its own
   [A-Za-z0-9_] name-character set -- so the two token classes
   (shellIsStatusVarToken below and shellParseVarRef above) can never
   both claim the same token. */
KTEST(shellParseVarRefRejectsQuestionMark)
{
    char name[16];
    KT_FALSE(t, shellParseVarRef("$?", name, sizeof(name)));
}

/* shellIsStatusVarToken (shell.c) is the pure-syntax half of the shell's
   new `$?` special parameter -- recognizing the EXACT two-character token
   "$?", never touching this file's own file-scope `lastStatus` (that
   substitution is shellExpandToken's job, not KTEST-able here for the
   same pre-scheduler reason shellExpandArgv's own env lookups are not). */
KTEST(shellIsStatusVarTokenAcceptsExactMatch)
{
    KT_TRUE(t, shellIsStatusVarToken("$?"));
}

KTEST(shellIsStatusVarTokenRejectsBareDollarSign)
{
    KT_FALSE(t, shellIsStatusVarToken("$"));
}

KTEST(shellIsStatusVarTokenRejectsBareQuestionMark)
{
    KT_FALSE(t, shellIsStatusVarToken("?"));
}

KTEST(shellIsStatusVarTokenRejectsTrailingGarbage)
{
    KT_FALSE(t, shellIsStatusVarToken("$?x"));
}

KTEST(shellIsStatusVarTokenRejectsLeadingGarbage)
{
    KT_FALSE(t, shellIsStatusVarToken("x$?"));
}

/* shellGlobMatch/shellTokenHasGlob/shellSplitGlobToken (shell.c) are the
   pure-logic half of the shell's new wildcard glob expansion
   (shellExpandGlobs, which actually walks the VFS and is not KTEST-able
   here for the same pre-scheduler reason shellExpandArgv's own VFS/thread
   lookups are not) -- the same "pure logic here, thread/VFS-touching glue
   stays static in shell.c" split every other pure function above already
   follows. */

KTEST(shellGlobMatchExactLiteral)
{
    KT_TRUE(t, shellGlobMatch("hello", "hello"));
    KT_FALSE(t, shellGlobMatch("hello", "hellox"));
    KT_FALSE(t, shellGlobMatch("hello", "hell"));
}

KTEST(shellGlobMatchStarMatchesAnySuffix)
{
    KT_TRUE(t, shellGlobMatch("a*", "a"));
    KT_TRUE(t, shellGlobMatch("a*", "abc"));
    KT_FALSE(t, shellGlobMatch("a*", "xabc"));
}

KTEST(shellGlobMatchStarMatchesAnyPrefix)
{
    KT_TRUE(t, shellGlobMatch("*.txt", "a.txt"));
    KT_TRUE(t, shellGlobMatch("*.txt", ".txt"));
    KT_FALSE(t, shellGlobMatch("*.txt", "a.log"));
}

KTEST(shellGlobMatchStarInTheMiddle)
{
    KT_TRUE(t, shellGlobMatch("a*b", "ab"));
    KT_TRUE(t, shellGlobMatch("a*b", "axxxb"));
    KT_FALSE(t, shellGlobMatch("a*b", "axxxc"));
}

KTEST(shellGlobMatchMultipleStars)
{
    KT_TRUE(t, shellGlobMatch("*a*b*", "aXbY"));
    KT_TRUE(t, shellGlobMatch("*a*b*", "ab"));
    KT_FALSE(t, shellGlobMatch("*a*b*", "ba"));
}

KTEST(shellGlobMatchBareStarMatchesEverythingIncludingEmpty)
{
    KT_TRUE(t, shellGlobMatch("*", ""));
    KT_TRUE(t, shellGlobMatch("*", "anything"));
}

KTEST(shellGlobMatchQuestionMarkMatchesExactlyOneChar)
{
    KT_TRUE(t, shellGlobMatch("a?c", "abc"));
    KT_FALSE(t, shellGlobMatch("a?c", "ac"));
    KT_FALSE(t, shellGlobMatch("a?c", "abbc"));
}

KTEST(shellGlobMatchEmptyPatternOnlyMatchesEmptyName)
{
    KT_TRUE(t, shellGlobMatch("", ""));
    KT_FALSE(t, shellGlobMatch("", "x"));
}

KTEST(shellTokenHasGlobDetectsStarAndQuestionMark)
{
    KT_TRUE(t, shellTokenHasGlob("*.txt"));
    KT_TRUE(t, shellTokenHasGlob("file?.log"));
    KT_FALSE(t, shellTokenHasGlob("plain.txt"));
    KT_FALSE(t, shellTokenHasGlob(""));
}

KTEST(shellSplitGlobTokenNoSlashYieldsEmptyDir)
{
    char dir[64];
    char pattern[64];
    shellSplitGlobToken("*.txt", dir, sizeof(dir), pattern, sizeof(pattern));
    KT_STREQ(t, dir, "");
    KT_STREQ(t, pattern, "*.txt");
}

KTEST(shellSplitGlobTokenSplitsAtLastSlashIncludingIt)
{
    char dir[64];
    char pattern[64];
    shellSplitGlobToken("/tmp/gd/*.txt", dir, sizeof(dir), pattern, sizeof(pattern));
    KT_STREQ(t, dir, "/tmp/gd/");
    KT_STREQ(t, pattern, "*.txt");
}

KTEST(shellSplitGlobTokenNoGlobCharStillSplitsCleanly)
{
    /* shellSplitGlobToken itself does not care whether the pattern half
       actually contains a glob character -- that gate is
       shellTokenHasGlob's own job, called first by shellExpandGlobs. */
    char dir[64];
    char pattern[64];
    shellSplitGlobToken("/tmp/gd/plain.txt", dir, sizeof(dir), pattern, sizeof(pattern));
    KT_STREQ(t, dir, "/tmp/gd/");
    KT_STREQ(t, pattern, "plain.txt");
}

/* shellFindCommandSubst (shell.c) is the pure-syntax half of the shell's
   new `$(cmd [args])` command-substitution expansion -- the impure glue
   that actually spawns and captures a command's output (shellExpandCommandSubst,
   static in shell.c) needs a real jobctl spawn, off limits for KTEST's
   pre-scheduler context, so only the locate-and-extract half is covered
   here, the same "pure logic here, thread/VFS-touching glue stays static
   in shell.c" split every other shell.h syntax function already gets. */

KTEST(shellFindCommandSubstNoDollarParenReturnsNotFound)
{
    int start, end;
    char cmd[32];
    KT_TRUE(t, shellFindCommandSubst("echo hello", &start, &end, cmd, sizeof(cmd)));
    KT_EQ(t, start, -1);
}

KTEST(shellFindCommandSubstWholeToken)
{
    int start, end;
    char cmd[32];
    KT_TRUE(t, shellFindCommandSubst("$(date)", &start, &end, cmd, sizeof(cmd)));
    KT_EQ(t, start, 0);
    KT_EQ(t, end, 7);
    KT_STREQ(t, cmd, "date");
}

KTEST(shellFindCommandSubstEmbeddedInLargerLine)
{
    int start, end;
    char cmd[32];
    KT_TRUE(t, shellFindCommandSubst("echo $(date) done", &start, &end, cmd, sizeof(cmd)));
    KT_EQ(t, start, 5);
    KT_EQ(t, end, 12);
    KT_STREQ(t, cmd, "date");
}

KTEST(shellFindCommandSubstCapturesMultiWordCommand)
{
    int start, end;
    char cmd[32];
    KT_TRUE(t, shellFindCommandSubst("argecho $(argecho a b)", &start, &end, cmd, sizeof(cmd)));
    KT_STREQ(t, cmd, "argecho a b");
}

KTEST(shellFindCommandSubstUnterminatedIsMalformed)
{
    int start, end;
    char cmd[32];
    KT_FALSE(t, shellFindCommandSubst("echo $(date", &start, &end, cmd, sizeof(cmd)));
}

KTEST(shellFindCommandSubstEmptyParensYieldsEmptyCommand)
{
    int start, end;
    char cmd[32];
    KT_TRUE(t, shellFindCommandSubst("$()", &start, &end, cmd, sizeof(cmd)));
    KT_EQ(t, start, 0);
    KT_EQ(t, end, 3);
    KT_STREQ(t, cmd, "");
}

KTEST(shellFindCommandSubstTruncatesToCapacity)
{
    int start, end;
    char cmd[4];
    KT_TRUE(t, shellFindCommandSubst("$(longcommand)", &start, &end, cmd, sizeof(cmd)));
    KT_STREQ(t, cmd, "lon");
}

KTEST(shellFindCommandSubstOnlyMatchesFirstOccurrence)
{
    /* No nested-paren support (documented in shell.h) -- the very NEXT ')'
       after the opening '$(' ends the match, so a second, unrelated
       `$(...)` later in the line is left completely untouched for a
       caller's own repeat scan (shellExpandCommandSubst's own loop) to
       find on a later pass, never folded into this one's own cmdOut. */
    int start, end;
    char cmd[32];
    KT_TRUE(t, shellFindCommandSubst("$(a) $(b)", &start, &end, cmd, sizeof(cmd)));
    KT_EQ(t, start, 0);
    KT_EQ(t, end, 4);
    KT_STREQ(t, cmd, "a");
}
