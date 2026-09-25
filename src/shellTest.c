#include "ktest.h"
#include "shell.h"
#include "string.h"

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
    int count = shellSplitPipelineStages(text, stages, shellMaxPipelineStages);
    KT_EQ(t, count, -1);
    KT_STREQ(t, text, "a | b | c | d | e");
}

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
    int count = shellSplitAndOr(text, segments, ops, shellMaxAndOrSegments);
    KT_EQ(t, count, -1);
    KT_STREQ(t, text, "a && b && c && d && e");
}

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

KTEST(shellParseVarRefRejectsQuestionMark)
{
    char name[16];
    KT_FALSE(t, shellParseVarRef("$?", name, sizeof(name)));
}

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
    char dir[64];
    char pattern[64];
    shellSplitGlobToken("/tmp/gd/plain.txt", dir, sizeof(dir), pattern, sizeof(pattern));
    KT_STREQ(t, dir, "/tmp/gd/");
    KT_STREQ(t, pattern, "plain.txt");
}

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
    int start, end;
    char cmd[32];
    KT_TRUE(t, shellFindCommandSubst("$(a) $(b)", &start, &end, cmd, sizeof(cmd)));
    KT_EQ(t, start, 0);
    KT_EQ(t, end, 4);
    KT_STREQ(t, cmd, "a");
}
