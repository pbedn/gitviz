#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define main gitviz_app_main
#include "../gitviz.c"
#undef main

static int tests_run = 0;

#define EXPECT(cond) do { \
    tests_run++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

static void run_cmd_or_die(const char *cmd)
{
    int rc = system(cmd);
    if (rc != 0)
    {
        fprintf(stderr, "Command failed (%d): %s\n", rc, cmd);
        exit(1);
    }
}

static void test_utils(void)
{
    char s1[64] = "abc\n\r";
    TrimTrailingNewline(s1);
    EXPECT(strcmp(s1, "abc") == 0);

    char s2[6];
    CopyBounded(s2, sizeof(s2), "hello-world");
    EXPECT(strcmp(s2, "hello") == 0);

    char q[128];
    ShellQuote(q, sizeof(q), "a'b c");
    EXPECT(strstr(q, "'\\''") != NULL);

    char p[128];
    JoinPath(p, sizeof(p), "/tmp", "abc");
    EXPECT(strcmp(p, "/tmp/abc") == 0);

    strcpy(p, "/tmp///");
    TrimTrailingSlash(p);
    EXPECT(strcmp(p, "/tmp") == 0);

    strcpy(p, "/tmp/a/b");
    GoParentPath(p, sizeof(p));
    EXPECT(strcmp(p, "/tmp/a") == 0);
}

static void test_cli_parsing(void)
{
    const char *repo = NULL;
    bool help = false;

    char *argv1[] = { "gitviz", "-h" };
    EXPECT(ParseArgs(2, argv1, &repo, &help) == true);
    EXPECT(help == true);

    help = false;
    char *argv2[] = { "gitviz", "--repo", "/tmp/x" };
    EXPECT(ParseArgs(3, argv2, &repo, &help) == true);
    EXPECT(strcmp(repo, "/tmp/x") == 0);

    help = false;
    char *argv3[] = { "gitviz", "--bad" };
    EXPECT(ParseArgs(2, argv3, &repo, &help) == false);
}

static void test_diff_parser(void)
{
    const char *txt =
        "diff --git a/a.txt b/a.txt\n"
        "index 111..222 100644\n"
        "--- a/a.txt\n"
        "+++ b/a.txt\n"
        "@@ -1 +1,2 @@\n"
        "-old\n"
        "+new\n"
        "+next\n"
        "diff --git a/b.txt b/b.txt\n"
        "new file mode 100644\n"
        "@@ -0,0 +1 @@\n"
        "+hello\n";

    FILE *fp = tmpfile();
    EXPECT(fp != NULL);
    fputs(txt, fp);
    rewind(fp);

    ParseDiffStream(fp);
    fclose(fp);

    EXPECT(parsedDiff.fileCount == 2);
    EXPECT(strcmp(parsedDiff.files[0].path, "a.txt") == 0);
    EXPECT(strcmp(parsedDiff.files[1].path, "b.txt") == 0);
    EXPECT(parsedDiff.hunkCount == 2);
    EXPECT(parsedDiff.lineCount > 0);

    ResetDiffPanelUiState();
    EXPECT(GetPanelVisualLines(0) > 0);
    panelCollapsed[0] = true;
    EXPECT(GetPanelVisualLines(0) == 2);
    panelCollapsed[0] = false;
    EXPECT(GetVisualLineForPanelTop(1) > 0);
    EXPECT(GetVisualLineForHunk(1) >= 0);
    EXPECT(FindClosestHunkForPanel(1) >= 0);
}

static void test_repo_integration(void)
{
    char tdir[] = "/tmp/gitviz-test-XXXXXX";
    char *repo = mkdtemp(tdir);
    EXPECT(repo != NULL);

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "git -C %s init -q", repo);
    run_cmd_or_die(cmd);
    snprintf(cmd, sizeof(cmd), "git -C %s config user.email 'test@example.com'", repo);
    run_cmd_or_die(cmd);
    snprintf(cmd, sizeof(cmd), "git -C %s config user.name 'Test User'", repo);
    run_cmd_or_die(cmd);

    snprintf(cmd, sizeof(cmd), "sh -c 'echo one > %s/a.txt'", repo);
    run_cmd_or_die(cmd);
    snprintf(cmd, sizeof(cmd), "git -C %s add a.txt && git -C %s commit -q -m init", repo, repo);
    run_cmd_or_die(cmd);

    snprintf(cmd, sizeof(cmd), "sh -c 'echo two >> %s/a.txt'", repo);
    run_cmd_or_die(cmd);
    snprintf(cmd, sizeof(cmd), "sh -c 'echo staged > %s/b.txt'", repo);
    run_cmd_or_die(cmd);
    snprintf(cmd, sizeof(cmd), "git -C %s add b.txt", repo);
    run_cmd_or_die(cmd);

    EXPECT(ResolveRepoRoot(repo) == true);
    EXPECT(strcmp(repoRoot, repo) == 0);

    LoadTimeline();
    EXPECT(timelineCount >= 3);
    EXPECT(timeline[0].type == TIMELINE_UNSTAGED);
    EXPECT(timeline[1].type == TIMELINE_STAGED);
    EXPECT(unstagedFilesCount >= 1);
    EXPECT(stagedFilesCount >= 1);

    LoadDiffForSelection(0);
    EXPECT(parsedDiff.fileCount >= 1);

    LoadDiffForSelection(1);
    EXPECT(parsedDiff.fileCount >= 1);

    LoadDiffForSelection(2);
    EXPECT(parsedDiff.lineCount >= 1);

    selected = 1;
    RefreshTimelineAndSelection();
    EXPECT(selected == 1 || timelineCount > 1);
}

int main(void)
{
    test_utils();
    test_cli_parsing();
    test_diff_parser();
    test_repo_integration();

    printf("OK: %d checks passed\n", tests_run);
    return 0;
}
