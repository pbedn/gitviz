/********************************************************************
 Git Commit + Diff Visualizer
********************************************************************/

#include "raylib.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-result"
#endif
#define RAYGUI_IMPLEMENTATION
#include "raygui.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef Clamp
#define Clamp(value, min, max) \
    ((value) < (min) ? (min) : ((value) > (max) ? (max) : (value)))
#endif

#define MAX_LINE          1024
#define MAX_DIFF_LINES    40000
#define MAX_DIFF_FILES    1024
#define MAX_HUNKS         4096
#define MAX_TIMELINE      512
#define MAX_DIR_ITEMS     256
#define MAX_COMMIT_TITLE  256

typedef enum {
    TIMELINE_UNSTAGED = 0,
    TIMELINE_STAGED   = 1,
    TIMELINE_COMMIT   = 2
} TimelineType;

typedef struct {
    TimelineType type;
    char hash[16];
    char title[MAX_COMMIT_TITLE];
} TimelineItem;

typedef struct {
    char type;
    char text[MAX_LINE];
} DiffLine;

typedef struct {
    char path[512];
    int lineStart;
    int lineCount;
} DiffFilePanel;

typedef struct {
    int fileIndex;
    int lineInFile;
} DiffHunkRef;

typedef struct {
    DiffLine lines[MAX_DIFF_LINES];
    int lineCount;
    DiffFilePanel files[MAX_DIFF_FILES];
    int fileCount;
    DiffHunkRef hunks[MAX_HUNKS];
    int hunkCount;
} ParsedDiff;

static TimelineItem timeline[MAX_TIMELINE];
static int timelineCount = 0;
static ParsedDiff parsedDiff;
static char statusHint[128];
static char repoRoot[512];
static char branchName[128];
static int unstagedFilesCount = 0;
static int stagedFilesCount = 0;
static int untrackedFilesCount = 0;
static bool repoInputActive = false;
static char repoInputPath[512];
static char repoInputHint[128];
static bool repoInputEditMode = true;
static char pickerItems[MAX_DIR_ITEMS][512];
static const char *pickerItemPtrs[MAX_DIR_ITEMS];
static int pickerItemCount = 0;
static int pickerScrollIndex = 0;
static int pickerActive = -1;
static int pickerFocus = -1;

static int selected = 0;
static int hover    = -1;

static float fontSize = 16.0f;
static float lineStep = 18.0f;

static int commitScroll = 0;
static int diffScroll   = 0;
static int leftPaneWidth = 430;
static bool dragLeftScrollbar = false;
static bool dragRightScrollbar = false;
static bool dragPaneSplitter = false;
static float dragLeftGrabY = 0.0f;
static float dragRightGrabY = 0.0f;
static float dragSplitterOffsetX = 0.0f;
static bool panelCollapsed[MAX_DIFF_FILES];
static int activePanel = 0;
static int activeHunk = 0;

static Font font;
static Font fontSmall;
static bool fontOwned = false;
static bool fontSmallOwned = false;

/* ------------------------------------------------------------ */

// Remove trailing newline and carriage-return characters in-place.
static void TrimTrailingNewline(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
    {
        s[n - 1] = 0;
        n--;
    }
}

// Copy src into dst with explicit NUL termination and size bounds.
static void CopyBounded(char *dst, size_t dstSize, const char *src)
{
    if (dstSize == 0) return;
    size_t n = strlen(src);
    if (n >= dstSize) n = dstSize - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

// Wrap a path/argument in single quotes for shell-safe command building.
// Escapes inner single quotes using POSIX-compatible pattern.
static void ShellQuote(char *dst, size_t dstSize, const char *src)
{
    size_t d = 0;
    if (dstSize == 0) return;
    if (d + 1 < dstSize) dst[d++] = '\'';
    for (size_t i = 0; src[i] != 0 && d + 1 < dstSize; i++)
    {
        if (src[i] == '\'')
        {
            const char *esc = "'\\''";
            for (size_t j = 0; esc[j] != 0 && d + 1 < dstSize; j++)
                dst[d++] = esc[j];
        }
        else
        {
            dst[d++] = src[i];
        }
    }
    if (d + 1 < dstSize) dst[d++] = '\'';
    dst[d] = 0;
}

// Return true if path exists and is a directory.
static bool IsDirectoryPath(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0) && S_ISDIR(st.st_mode);
}

// Join base/name into dst unless name is already absolute.
static void JoinPath(char *dst, size_t dstSize, const char *base, const char *name)
{
    if ((name[0] == '/') || (base[0] == 0))
    {
        CopyBounded(dst, dstSize, name);
        return;
    }
    snprintf(dst, dstSize, "%s/%s", base, name);
}

// Trim trailing slashes while keeping root '/' intact.
static void TrimTrailingSlash(char *path)
{
    size_t n = strlen(path);
    while ((n > 1) && (path[n - 1] == '/'))
    {
        path[n - 1] = 0;
        n--;
    }
}

// Replace path with its parent directory.
// Falls back to '.' when no slash is present.
static void GoParentPath(char *path, size_t pathSize)
{
    TrimTrailingSlash(path);
    char *slash = strrchr(path, '/');
    if (!slash)
    {
        CopyBounded(path, pathSize, ".");
        return;
    }
    if (slash == path) slash[1] = 0;
    else *slash = 0;
}

// qsort comparator for lexicographic string ordering.
static int CompareStrings(const void *a, const void *b)
{
    const char *sa = (const char *)a;
    const char *sb = (const char *)b;
    return strcmp(sa, sb);
}

// Populate repository picker with immediate child directories.
// Side effects: resets picker selection/scroll state.
static void RefreshPickerItems(void)
{
    pickerItemCount = 0;
    pickerActive = -1;
    pickerFocus = -1;
    pickerScrollIndex = 0;

    if (repoInputPath[0] == 0) CopyBounded(repoInputPath, sizeof(repoInputPath), ".");

    DIR *dir = opendir(repoInputPath);
    if (!dir)
    {
        CopyBounded(repoInputHint, sizeof(repoInputHint), "Cannot open directory");
        return;
    }

    if (strcmp(repoInputPath, "/") != 0)
    {
        CopyBounded(pickerItems[pickerItemCount++], sizeof(pickerItems[0]), "..");
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && pickerItemCount < MAX_DIR_ITEMS)
    {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        char full[1024];
        JoinPath(full, sizeof(full), repoInputPath, ent->d_name);
        if (IsDirectoryPath(full))
        {
            CopyBounded(pickerItems[pickerItemCount], sizeof(pickerItems[0]), ent->d_name);
            pickerItemCount++;
        }
    }
    closedir(dir);

    if (pickerItemCount > 1)
    {
        qsort(&pickerItems[1], (size_t)(pickerItemCount - 1), sizeof(pickerItems[0]), CompareStrings);
    }

    for (int i = 0; i < pickerItemCount; i++) pickerItemPtrs[i] = pickerItems[i];
    repoInputHint[0] = 0;
}

// Move picker path one directory up and refresh list.
static void PickerGoParent(void)
{
    GoParentPath(repoInputPath, sizeof(repoInputPath));
    RefreshPickerItems();
}

// Enter currently selected picker directory.
// Special-case '..' to move to parent.
static void PickerOpenSelected(void)
{
    if (pickerActive < 0 || pickerActive >= pickerItemCount)
    {
        CopyBounded(repoInputHint, sizeof(repoInputHint), "Select a directory first");
        return;
    }
    if (strcmp(pickerItems[pickerActive], "..") == 0)
    {
        PickerGoParent();
        return;
    }

    char next[1024];
    JoinPath(next, sizeof(next), repoInputPath, pickerItems[pickerActive]);
    if (!IsDirectoryPath(next))
    {
        CopyBounded(repoInputHint, sizeof(repoInputHint), "Selected item is not a directory");
        return;
    }

    CopyBounded(repoInputPath, sizeof(repoInputPath), next);
    RefreshPickerItems();
}

// Resolve startPath to git top-level directory.
// Side effects: updates repoRoot on success.
static bool ResolveRepoRoot(const char *startPath)
{
    char qPath[1200];
    char cmd[1400];
    char line[MAX_LINE];

    ShellQuote(qPath, sizeof(qPath), startPath);
    snprintf(cmd, sizeof(cmd), "git -C %s rev-parse --show-toplevel 2>/dev/null", qPath);

    FILE *fp = popen(cmd, "r");
    if (!fp) return false;

    char found[512] = { 0 };
    if (fgets(line, sizeof(line), fp))
    {
        TrimTrailingNewline(line);
        CopyBounded(found, sizeof(found), line);
    }

    int rc = pclose(fp);
    if (rc == 0 && found[0] != 0)
    {
        CopyBounded(repoRoot, sizeof(repoRoot), found);
        return true;
    }
    return false;
}

// Print command-line help and available runtime shortcuts.
static void PrintUsage(const char *prog)
{
    printf("Usage: %s [repo-path]\n", prog);
    printf("       %s -r <repo-path>\n", prog);
    printf("       %s --repo <repo-path>\n\n", prog);
    printf("Options:\n");
    printf("  -h, --help          Show this help and exit\n");
    printf("  -r, --repo <path>   Observe the git repository at <path>\n\n");
    printf("Runtime keys:\n");
    printf("  Up/Down             Select timeline item\n");
    printf("  Mouse wheel         Scroll panel under cursor\n");
    printf("  R                   Refresh git status/diff\n");
    printf("  Ctrl + O            Open repository switcher\n");
    printf("  Ctrl + '+' / '-'    Increase/decrease font size\n");
}

// Parse CLI arguments for help and repository path options.
// Supports positional path and -r/--repo.
static bool ParseArgs(int argc, char **argv, const char **repoArg, bool *showHelp)
{
    *repoArg = ".";
    *showHelp = false;

    for (int i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0)
        {
            *showHelp = true;
            return true;
        }
        if (strcmp(a, "-r") == 0 || strcmp(a, "--repo") == 0)
        {
            if (i + 1 >= argc) return false;
            *repoArg = argv[++i];
            continue;
        }
        if (a[0] == '-')
            return false;
        *repoArg = a;
    }
    return true;
}

// Count output lines from a shell command.
static int CountLinesInCommand(const char *cmd)
{
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;
    int count = 0;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), fp)) count++;
    pclose(fp);
    return count;
}

// Reset per-diff UI state after selection changes.
static void ResetDiffPanelUiState(void)
{
    for (int i = 0; i < MAX_DIFF_FILES; i++) panelCollapsed[i] = false;
    activePanel = 0;
    activeHunk = 0;
}

// Return visual line count for a panel, accounting for collapsed state.
static int GetPanelVisualLines(int panelIndex)
{
    if (panelIndex < 0 || panelIndex >= parsedDiff.fileCount) return 0;
    int body = panelCollapsed[panelIndex] ? 0 : parsedDiff.files[panelIndex].lineCount;
    return 2 + body; // header + spacer + optional body
}

// Return cumulative visual line offset at top of target panel.
static int GetVisualLineForPanelTop(int panelIndex)
{
    int lines = 0;
    for (int i = 0; i < panelIndex && i < parsedDiff.fileCount; i++)
        lines += GetPanelVisualLines(i);
    return lines;
}

// Map hunk index to global visual line offset for quick scrolling.
static int GetVisualLineForHunk(int hunkIndex)
{
    if (hunkIndex < 0 || hunkIndex >= parsedDiff.hunkCount) return 0;
    DiffHunkRef *h = &parsedDiff.hunks[hunkIndex];
    return GetVisualLineForPanelTop(h->fileIndex) + 1 + h->lineInFile;
}

// Find closest hunk index to a given panel index.
static int FindClosestHunkForPanel(int panelIndex)
{
    int best = -1;
    int bestDist = 1 << 30;
    for (int i = 0; i < parsedDiff.hunkCount; i++)
    {
        int d = parsedDiff.hunks[i].fileIndex - panelIndex;
        if (d < 0) d = -d;
        if (d < bestDist)
        {
            best = i;
            bestDist = d;
        }
    }
    return best;
}

// Read first line from command output into caller-provided buffer.
static bool ReadFirstLineFromCommand(const char *cmd, char *out, size_t outSize)
{
    FILE *fp = popen(cmd, "r");
    if (!fp) return false;
    char line[MAX_LINE];
    bool ok = false;
    if (fgets(line, sizeof(line), fp))
    {
        TrimTrailingNewline(line);
        CopyBounded(out, outSize, line);
        ok = true;
    }
    pclose(fp);
    return ok;
}

// Add a new diff file panel and return its index.
static int AddDiffPanel(const char *path)
{
    if (parsedDiff.fileCount >= MAX_DIFF_FILES) return -1;
    DiffFilePanel *p = &parsedDiff.files[parsedDiff.fileCount++];
    memset(p, 0, sizeof(*p));
    CopyBounded(p->path, sizeof(p->path), path);
    p->lineStart = parsedDiff.lineCount;
    return parsedDiff.fileCount - 1;
}

// Extract display file path from a 'diff --git a/... b/...' header.
static void ParsePathFromDiffHeader(const char *line, char *out, size_t outSize)
{
    const char *b = strstr(line, " b/");
    if (b)
    {
        b += 3;
        size_t i = 0;
        while (b[i] && !isspace((unsigned char)b[i]) && i + 1 < outSize)
        {
            out[i] = b[i];
            i++;
        }
        out[i] = 0;
        return;
    }
    CopyBounded(out, outSize, "(file)");
}

// Parse git diff stream into file panels, lines, and hunk anchors.
// Assumes unified diff format with 'diff --git' boundaries.
static void ParseDiffStream(FILE *fp)
{
    parsedDiff.lineCount = 0;
    parsedDiff.fileCount = 0;
    parsedDiff.hunkCount = 0;

    char line[MAX_LINE];
    int currentPanel = -1;

    while (fgets(line, sizeof(line), fp) && parsedDiff.lineCount < MAX_DIFF_LINES)
    {
        TrimTrailingNewline(line);

        if (strncmp(line, "diff --git ", 11) == 0)
        {
            char path[512];
            ParsePathFromDiffHeader(line, path, sizeof(path));
            currentPanel = AddDiffPanel(path);
            continue;
        }

        if (currentPanel < 0) currentPanel = AddDiffPanel("(summary)");
        if (currentPanel < 0) break;

        if (strncmp(line, "@@ ", 3) == 0 && parsedDiff.hunkCount < MAX_HUNKS)
        {
            parsedDiff.hunks[parsedDiff.hunkCount].fileIndex = currentPanel;
            parsedDiff.hunks[parsedDiff.hunkCount].lineInFile = parsedDiff.files[currentPanel].lineCount;
            parsedDiff.hunkCount++;
        }

        DiffLine *dl = &parsedDiff.lines[parsedDiff.lineCount++];
        dl->type = line[0] ? line[0] : ' ';
        CopyBounded(dl->text, sizeof(dl->text), line);
        parsedDiff.files[currentPanel].lineCount++;
    }
}

// Build left-pane timeline and status counters from current repo state.
static void LoadTimeline(void)
{
    timelineCount = 0;
    if (repoRoot[0] == 0)
    {
        CopyBounded(statusHint, sizeof(statusHint), "No repository selected");
        return;
    }

    char qRepo[1200];
    char cmd[1600];
    ShellQuote(qRepo, sizeof(qRepo), repoRoot);

    unstagedFilesCount = 0;
    stagedFilesCount = 0;
    untrackedFilesCount = 0;
    branchName[0] = 0;

    snprintf(cmd, sizeof(cmd), "git -C %s rev-parse --abbrev-ref HEAD", qRepo);
    if (!ReadFirstLineFromCommand(cmd, branchName, sizeof(branchName)))
        CopyBounded(branchName, sizeof(branchName), "(unknown)");

    snprintf(cmd, sizeof(cmd), "git -C %s diff --name-only", qRepo);
    unstagedFilesCount = CountLinesInCommand(cmd);
    snprintf(cmd, sizeof(cmd), "git -C %s diff --cached --name-only", qRepo);
    stagedFilesCount = CountLinesInCommand(cmd);
    snprintf(cmd, sizeof(cmd), "git -C %s ls-files --others --exclude-standard", qRepo);
    untrackedFilesCount = CountLinesInCommand(cmd);

    timeline[timelineCount].type = TIMELINE_UNSTAGED;
    timeline[timelineCount].hash[0] = 0;
    snprintf(timeline[timelineCount].title, sizeof(timeline[timelineCount].title),
             "Unstaged Files (%d)", unstagedFilesCount);
    timelineCount++;

    timeline[timelineCount].type = TIMELINE_STAGED;
    timeline[timelineCount].hash[0] = 0;
    snprintf(timeline[timelineCount].title, sizeof(timeline[timelineCount].title),
             "Staged Files (%d)", stagedFilesCount);
    timelineCount++;

    snprintf(cmd, sizeof(cmd), "git -C %s log --oneline --max-count=%d", qRepo, MAX_TIMELINE - 2);
    FILE *fp = popen(cmd, "r");
    if (!fp)
    {
        CopyBounded(statusHint, sizeof(statusHint), "Failed to load commit history");
        return;
    }

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), fp) && timelineCount < MAX_TIMELINE)
    {
        TrimTrailingNewline(line);
        char *space = strchr(line, ' ');
        if (!space) continue;
        *space = 0;
        TimelineItem *it = &timeline[timelineCount++];
        it->type = TIMELINE_COMMIT;
        CopyBounded(it->hash, sizeof(it->hash), line);
        CopyBounded(it->title, sizeof(it->title), space + 1);
    }
    pclose(fp);
}

// Load diff content for currently selected timeline item.
// Side effects: resets parsed diff and panel UI state.
static void LoadDiffForSelection(int index)
{
    parsedDiff.lineCount = 0;
    parsedDiff.fileCount = 0;
    parsedDiff.hunkCount = 0;

    if (index < 0 || index >= timelineCount || repoRoot[0] == 0) return;

    char qRepo[1200];
    char cmd[2200];
    ShellQuote(qRepo, sizeof(qRepo), repoRoot);
    const TimelineItem *it = &timeline[index];

    if (it->type == TIMELINE_UNSTAGED)
        snprintf(cmd, sizeof(cmd), "git -C %s diff --no-color", qRepo);
    else if (it->type == TIMELINE_STAGED)
        snprintf(cmd, sizeof(cmd), "git -C %s diff --cached --no-color", qRepo);
    else
        snprintf(cmd, sizeof(cmd), "git -C %s show --no-color --pretty=format: %s", qRepo, it->hash);

    FILE *fp = popen(cmd, "r");
    if (!fp)
    {
        CopyBounded(statusHint, sizeof(statusHint), "Failed to load diff");
        return;
    }
    ParseDiffStream(fp);
    pclose(fp);
    ResetDiffPanelUiState();
}

/* ------------------------------------------------------------ */

// Switch active repository path and reload timeline/diff data.
static bool ReloadFromRepoPath(const char *path)
{
    if (!ResolveRepoRoot(path))
    {
        CopyBounded(statusHint, sizeof(statusHint), "Invalid repo path. Press Ctrl+O to try again.");
        return false;
    }

    SetWindowTitle(TextFormat("gitviz - %s", repoRoot));
    selected = 0;
    commitScroll = 0;
    diffScroll = 0;
    LoadTimeline();
    if (timelineCount > 0) LoadDiffForSelection(0);
    else parsedDiff.lineCount = 0;
    return true;
}

// Refresh timeline while preserving nearest prior selection.
static void RefreshTimelineAndSelection(void)
{
    TimelineType prevType = TIMELINE_UNSTAGED;
    char prevHash[16] = { 0 };
    if (selected >= 0 && selected < timelineCount)
    {
        prevType = timeline[selected].type;
        CopyBounded(prevHash, sizeof(prevHash), timeline[selected].hash);
    }

    LoadTimeline();
    selected = 0;
    for (int i = 0; i < timelineCount; i++)
    {
        if (timeline[i].type != prevType) continue;
        if (prevType == TIMELINE_COMMIT)
        {
            if (strcmp(timeline[i].hash, prevHash) == 0)
            {
                selected = i;
                break;
            }
        }
        else
        {
            selected = i;
            break;
        }
    }
    if (timelineCount > 0) LoadDiffForSelection(selected);
    else parsedDiff.lineCount = 0;
    diffScroll = 0;
}

/* ------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *startPath = ".";
    bool showHelp = false;
    if (!ParseArgs(argc, argv, &startPath, &showHelp))
    {
        PrintUsage(argv[0]);
        return 2;
    }
    if (showHelp)
    {
        PrintUsage(argv[0]);
        return 0;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(1200, 800, "gitviz");

    font = LoadFontEx("assets/fonts/UbuntuMono-R.ttf", 32, 0, 0);
    if (font.texture.id == 0)
    {
        TraceLog(LOG_WARNING, "Failed to load assets/fonts/UbuntuMono-R.ttf, using raylib default font");
        font = GetFontDefault();
        fontOwned = false;
    }
    else
    {
        SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
        fontOwned = true;
    }

    fontSmall = LoadFontEx("assets/fonts/Ubuntu-R.ttf", 24, 0, 0);
    if (fontSmall.texture.id == 0)
    {
        TraceLog(LOG_WARNING, "Failed to load assets/fonts/Ubuntu-R.ttf, using raylib default font");
        fontSmall = GetFontDefault();
        fontSmallOwned = false;
    }
    else
    {
        SetTextureFilter(fontSmall.texture, TEXTURE_FILTER_BILINEAR);
        fontSmallOwned = true;
    }

    if (!ReloadFromRepoPath(startPath))
    {
        timelineCount = 0;
        parsedDiff.lineCount = 0;
        parsedDiff.fileCount = 0;
    }

    SetTargetFPS(60);

    while (!WindowShouldClose())
    {
        /* ---------- Keyboard ---------- */

        if (IsKeyDown(KEY_LEFT_CONTROL))
        {
            if (IsKeyPressed(KEY_EQUAL))
                fontSize = Clamp(fontSize + 1, 10, 28);
            if (IsKeyPressed(KEY_MINUS))
                fontSize = Clamp(fontSize - 1, 10, 28);
        }

        float mainTextSize = fontSize;
        float mainTextSpacing = 1.0f;
        lineStep = mainTextSize * 1.35f;

        if ((IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) && IsKeyPressed(KEY_O))
        {
            repoInputActive = true;
            repoInputHint[0] = 0;
            repoInputEditMode = true;
            if (repoRoot[0] != 0)
                CopyBounded(repoInputPath, sizeof(repoInputPath), repoRoot);
            else
                CopyBounded(repoInputPath, sizeof(repoInputPath), ".");
            RefreshPickerItems();
        }

        if (repoInputActive)
        {
            if (IsKeyPressed(KEY_ESCAPE))
            {
                repoInputActive = false;
            }
            if (IsKeyPressed(KEY_ENTER))
            {
                if (repoInputPath[0] == 0)
                {
                    CopyBounded(repoInputHint, sizeof(repoInputHint), "Path cannot be empty");
                }
                else
                {
                    if (ReloadFromRepoPath(repoInputPath)) repoInputActive = false;
                    else CopyBounded(repoInputHint, sizeof(repoInputHint), "Not a git repository");
                }
            }
        }

        if (!repoInputActive && IsKeyPressed(KEY_DOWN) && selected < timelineCount - 1)
        {
            selected++;
            diffScroll = 0;
            LoadDiffForSelection(selected);
        }
        if (!repoInputActive && IsKeyPressed(KEY_UP) && selected > 0)
        {
            selected--;
            diffScroll = 0;
            LoadDiffForSelection(selected);
        }
        if (!repoInputActive && IsKeyPressed(KEY_R))
        {
            RefreshTimelineAndSelection();
        }
        if (!repoInputActive && IsKeyPressed(KEY_J) && parsedDiff.fileCount > 0)
        {
            if (activePanel < parsedDiff.fileCount - 1) activePanel++;
            diffScroll = GetVisualLineForPanelTop(activePanel);
            int h = FindClosestHunkForPanel(activePanel);
            if (h >= 0) activeHunk = h;
        }
        if (!repoInputActive && IsKeyPressed(KEY_K) && parsedDiff.fileCount > 0)
        {
            if (activePanel > 0) activePanel--;
            diffScroll = GetVisualLineForPanelTop(activePanel);
            int h = FindClosestHunkForPanel(activePanel);
            if (h >= 0) activeHunk = h;
        }
        if (!repoInputActive && IsKeyPressed(KEY_N) && parsedDiff.hunkCount > 0)
        {
            if (activeHunk < parsedDiff.hunkCount - 1) activeHunk++;
            DiffHunkRef *h = &parsedDiff.hunks[activeHunk];
            activePanel = h->fileIndex;
            panelCollapsed[activePanel] = false;
            diffScroll = GetVisualLineForHunk(activeHunk);
        }
        if (!repoInputActive && IsKeyPressed(KEY_P) && parsedDiff.hunkCount > 0)
        {
            if (activeHunk > 0) activeHunk--;
            DiffHunkRef *h = &parsedDiff.hunks[activeHunk];
            activePanel = h->fileIndex;
            panelCollapsed[activePanel] = false;
            diffScroll = GetVisualLineForHunk(activeHunk);
        }

        /* ---------- Mouse ---------- */

        Vector2 mouse = GetMousePosition();
        float wheel = GetMouseWheelMove();

        int headerHeight = 34;
        int listTop = headerHeight + 10;
        int bottomBarHeight = 28;
        int width = GetScreenWidth();
        int height = GetScreenHeight();
        int minLeftWidth = 260;
        int minRightWidth = 360;
        int maxLeftWidth = width - minRightWidth;
        if (maxLeftWidth < minLeftWidth) maxLeftWidth = minLeftWidth;
        int leftWidth = leftPaneWidth;
        if (leftWidth < minLeftWidth) leftWidth = minLeftWidth;
        if (leftWidth > maxLeftWidth) leftWidth = maxLeftWidth;
        leftPaneWidth = leftWidth;
        int listViewHeight = height - listTop - bottomBarHeight - 8;
        int diffViewHeight = height - listTop - bottomBarHeight - 8;
        if (listViewHeight < 1) listViewHeight = 1;
        if (diffViewHeight < 1) diffViewHeight = 1;

        int timelineContentPx = (int)(timelineCount * lineStep);
        int diffContentPx = 0;
        for (int f = 0; f < parsedDiff.fileCount; f++)
        {
            diffContentPx += (int)((float)GetPanelVisualLines(f) * lineStep);
        }
        if (diffContentPx == 0) diffContentPx = (int)lineStep;

        int maxTimelineScrollPx = timelineContentPx - listViewHeight;
        int maxDiffScrollPx = diffContentPx - diffViewHeight;
        if (maxTimelineScrollPx < 0) maxTimelineScrollPx = 0;
        if (maxDiffScrollPx < 0) maxDiffScrollPx = 0;
        int linePx = (int)(lineStep + 0.5f);
        if (linePx < 1) linePx = 1;
        int maxCommitScroll = (maxTimelineScrollPx + linePx - 1) / linePx;
        int maxDiffScroll = (maxDiffScrollPx + linePx - 1) / linePx;
        const int scrollBarWidth = 8;
        const int scrollBarInset = 2;
        const int scrollHitPad = 6;

        int leftTrackX = leftWidth - scrollBarWidth - scrollBarInset;
        int leftTrackY = listTop;
        int leftTrackH = listViewHeight;
        int leftThumbH = leftTrackH;
        int leftThumbY = leftTrackY;
        if (timelineContentPx > listViewHeight)
        {
            leftThumbH = (int)((float)listViewHeight * ((float)listViewHeight / (float)timelineContentPx));
            if (leftThumbH < 18) leftThumbH = 18;
            leftThumbY = leftTrackY + (int)((float)(leftTrackH - leftThumbH) * ((float)(commitScroll * lineStep) / (float)maxTimelineScrollPx));
            if (leftThumbY < leftTrackY) leftThumbY = leftTrackY;
            if (leftThumbY > leftTrackY + leftTrackH - leftThumbH) leftThumbY = leftTrackY + leftTrackH - leftThumbH;
        }
        Rectangle leftTrackRec = { (float)leftTrackX, (float)leftTrackY, (float)scrollBarWidth, (float)leftTrackH };
        Rectangle leftThumbRec = { (float)leftTrackX, (float)leftThumbY, (float)scrollBarWidth, (float)leftThumbH };
        Rectangle leftTrackHitRec = { leftTrackRec.x - scrollHitPad, leftTrackRec.y, leftTrackRec.width + 2*scrollHitPad, leftTrackRec.height };
        Rectangle leftThumbHitRec = { leftThumbRec.x - scrollHitPad, leftThumbRec.y - scrollHitPad, leftThumbRec.width + 2*scrollHitPad, leftThumbRec.height + 2*scrollHitPad };
        Rectangle splitterRec = { (float)leftWidth - 3.0f, 0.0f, 6.0f, (float)height };

        int rightTrackX = width - scrollBarWidth - scrollBarInset;
        int rightTrackY = listTop;
        int rightTrackH = diffViewHeight;
        int rightThumbH = rightTrackH;
        int rightThumbY = rightTrackY;
        if (diffContentPx > diffViewHeight)
        {
            rightThumbH = (int)((float)diffViewHeight * ((float)diffViewHeight / (float)diffContentPx));
            if (rightThumbH < 18) rightThumbH = 18;
            rightThumbY = rightTrackY + (int)((float)(rightTrackH - rightThumbH) * ((float)(diffScroll * lineStep) / (float)maxDiffScrollPx));
            if (rightThumbY < rightTrackY) rightThumbY = rightTrackY;
            if (rightThumbY > rightTrackY + rightTrackH - rightThumbH) rightThumbY = rightTrackY + rightTrackH - rightThumbH;
        }
        Rectangle rightTrackRec = { (float)rightTrackX, (float)rightTrackY, (float)scrollBarWidth, (float)rightTrackH };
        Rectangle rightThumbRec = { (float)rightTrackX, (float)rightThumbY, (float)scrollBarWidth, (float)rightThumbH };
        Rectangle rightTrackHitRec = { rightTrackRec.x - scrollHitPad, rightTrackRec.y, rightTrackRec.width + 2*scrollHitPad, rightTrackRec.height };
        Rectangle rightThumbHitRec = { rightThumbRec.x - scrollHitPad, rightThumbRec.y - scrollHitPad, rightThumbRec.width + 2*scrollHitPad, rightThumbRec.height + 2*scrollHitPad };

        if (!repoInputActive && !dragPaneSplitter && mouse.x < leftWidth)
            commitScroll -= (int)(wheel * 3);
        else if (!repoInputActive && !dragPaneSplitter)
            diffScroll -= (int)(wheel * 3);

        if (repoInputActive)
        {
            dragLeftScrollbar = false;
            dragRightScrollbar = false;
            dragPaneSplitter = false;
        }

        if (!repoInputActive && IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
        {
            if (CheckCollisionPointRec(mouse, splitterRec))
            {
                dragPaneSplitter = true;
                dragSplitterOffsetX = mouse.x - (float)leftWidth;
                dragLeftScrollbar = false;
                dragRightScrollbar = false;
            }
            else if (maxTimelineScrollPx > 0 && CheckCollisionPointRec(mouse, leftThumbHitRec))
            {
                dragLeftScrollbar = true;
                dragLeftGrabY = mouse.y - (float)leftThumbY;
            }
            else if (maxTimelineScrollPx > 0 && CheckCollisionPointRec(mouse, leftTrackHitRec))
            {
                float targetTop = mouse.y - (float)leftThumbH * 0.5f;
                if (targetTop < leftTrackY) targetTop = (float)leftTrackY;
                if (targetTop > leftTrackY + leftTrackH - leftThumbH) targetTop = (float)(leftTrackY + leftTrackH - leftThumbH);
                float ratio = (leftTrackH - leftThumbH) > 0 ? (targetTop - (float)leftTrackY)/(float)(leftTrackH - leftThumbH) : 0.0f;
                float scrollPx = ratio * (float)maxTimelineScrollPx;
                commitScroll = (int)((scrollPx / lineStep) + 0.5f);
            }
            else if (maxDiffScrollPx > 0 && CheckCollisionPointRec(mouse, rightThumbHitRec))
            {
                dragRightScrollbar = true;
                dragRightGrabY = mouse.y - (float)rightThumbY;
            }
            else if (maxDiffScrollPx > 0 && CheckCollisionPointRec(mouse, rightTrackHitRec))
            {
                float targetTop = mouse.y - (float)rightThumbH * 0.5f;
                if (targetTop < rightTrackY) targetTop = (float)rightTrackY;
                if (targetTop > rightTrackY + rightTrackH - rightThumbH) targetTop = (float)(rightTrackY + rightTrackH - rightThumbH);
                float ratio = (rightTrackH - rightThumbH) > 0 ? (targetTop - (float)rightTrackY)/(float)(rightTrackH - rightThumbH) : 0.0f;
                float scrollPx = ratio * (float)maxDiffScrollPx;
                diffScroll = (int)((scrollPx / lineStep) + 0.5f);
            }
            else if (mouse.x > (float)leftWidth && mouse.x < rightTrackRec.x)
            {
                float py = (float)listTop - diffScroll*lineStep;
                for (int f = 0; f < parsedDiff.fileCount; f++)
                {
                    Rectangle headerRec = { (float)leftWidth + 8, py, (float)width - leftWidth - 18, lineStep };
                    if (CheckCollisionPointRec(mouse, headerRec))
                    {
                        activePanel = f;
                        panelCollapsed[f] = !panelCollapsed[f];
                        int h = FindClosestHunkForPanel(f);
                        if (h >= 0) activeHunk = h;
                        break;
                    }
                    py += lineStep;
                    if (!panelCollapsed[f]) py += parsedDiff.files[f].lineCount * lineStep;
                    py += lineStep;
                }
            }
        }

        if (!repoInputActive && dragPaneSplitter && IsMouseButtonDown(MOUSE_LEFT_BUTTON))
        {
            int target = (int)(mouse.x - dragSplitterOffsetX + 0.5f);
            if (target < minLeftWidth) target = minLeftWidth;
            if (target > maxLeftWidth) target = maxLeftWidth;
            leftPaneWidth = target;
            leftWidth = target;
        }
        if (!repoInputActive && dragLeftScrollbar && IsMouseButtonDown(MOUSE_LEFT_BUTTON))
        {
            float targetTop = mouse.y - dragLeftGrabY;
            if (targetTop < leftTrackY) targetTop = (float)leftTrackY;
            if (targetTop > leftTrackY + leftTrackH - leftThumbH) targetTop = (float)(leftTrackY + leftTrackH - leftThumbH);
            float ratio = (leftTrackH - leftThumbH) > 0 ? (targetTop - (float)leftTrackY)/(float)(leftTrackH - leftThumbH) : 0.0f;
            float scrollPx = ratio * (float)maxTimelineScrollPx;
            commitScroll = (int)((scrollPx / lineStep) + 0.5f);
        }
        if (!repoInputActive && dragRightScrollbar && IsMouseButtonDown(MOUSE_LEFT_BUTTON))
        {
            float targetTop = mouse.y - dragRightGrabY;
            if (targetTop < rightTrackY) targetTop = (float)rightTrackY;
            if (targetTop > rightTrackY + rightTrackH - rightThumbH) targetTop = (float)(rightTrackY + rightTrackH - rightThumbH);
            float ratio = (rightTrackH - rightThumbH) > 0 ? (targetTop - (float)rightTrackY)/(float)(rightTrackH - rightThumbH) : 0.0f;
            float scrollPx = ratio * (float)maxDiffScrollPx;
            diffScroll = (int)((scrollPx / lineStep) + 0.5f);
        }
        if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON))
        {
            dragLeftScrollbar = false;
            dragRightScrollbar = false;
            dragPaneSplitter = false;
        }

        if (commitScroll < 0) commitScroll = 0;
        if (diffScroll < 0) diffScroll = 0;
        if (commitScroll > maxCommitScroll) commitScroll = maxCommitScroll;
        if (diffScroll > maxDiffScroll) diffScroll = maxDiffScroll;

        hover = -1;

        int y0 = listTop - commitScroll * lineStep;
        for (int i = 0; i < timelineCount; i++)
        {
            Rectangle r = {
                10, y0 + i * lineStep,
                leftWidth - 20, lineStep
            };

            if (!repoInputActive && !dragPaneSplitter && !CheckCollisionPointRec(mouse, leftTrackRec) && CheckCollisionPointRec(mouse, r))
            {
                hover = i;
                if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
                {
                    selected = i;
                    diffScroll = 0;
                    LoadDiffForSelection(i);
                }
            }
        }

        /* ---------- Drawing ---------- */

        BeginDrawing();
        Color bgMain      = (Color){ 18, 24, 30, 255 };
        Color bgSidebar   = (Color){ 28, 36, 45, 255 };
        Color bgDiff      = (Color){ 14, 19, 24, 255 };
        Color rowHover    = (Color){ 53, 68, 82, 255 };
        Color rowSelected = (Color){ 74, 98, 120, 255 };
        Color textMain    = (Color){ 232, 239, 245, 255 };
        Color textHash    = (Color){ 153, 194, 230, 255 };
        Color divider     = (Color){ 68, 84, 97, 255 };
        Color plusColor   = (Color){ 115, 196, 140, 255 };
        Color minusColor  = (Color){ 232, 128, 128, 255 };

        ClearBackground(bgMain);

        DrawRectangle(0, 0, leftWidth, height, bgSidebar);
        DrawRectangle(0, 0, leftWidth, headerHeight, (Color){ 34, 45, 56, 255 });
        DrawTextEx(font, "TIMELINE", (Vector2){ 12, 8 }, mainTextSize, mainTextSpacing, textHash);
        DrawLineEx((Vector2){ leftWidth, 0 },
                   (Vector2){ leftWidth, height }, 2.0f, divider);
        bool splitterHover = CheckCollisionPointRec(mouse, splitterRec);
        Color splitterColor = divider;
        if (dragPaneSplitter) splitterColor = (Color){ 130, 155, 176, 255 };
        else if (splitterHover) splitterColor = (Color){ 104, 128, 146, 255 };
        DrawRectangle((int)splitterRec.x, 0, (int)splitterRec.width, height, (Color){ 21, 28, 35, 255 });
        DrawLineEx((Vector2){ leftWidth, 0 }, (Vector2){ leftWidth, height }, 2.0f, splitterColor);
        DrawRectangle(leftWidth, 0, width - leftWidth, headerHeight, (Color){ 20, 28, 34, 255 });
        if (timelineCount > 0)
        {
            if (timeline[selected].type == TIMELINE_COMMIT)
                DrawTextEx(font, TextFormat("DIFF %s  %s", timeline[selected].hash, timeline[selected].title),
                           (Vector2){ leftWidth + 12, 8 }, mainTextSize, mainTextSpacing, textHash);
            else
                DrawTextEx(font, TextFormat("DIFF %s", timeline[selected].title),
                           (Vector2){ leftWidth + 12, 8 }, mainTextSize, mainTextSpacing, textHash);
        }
        else
            DrawTextEx(font, "DIFF", (Vector2){ leftWidth + 12, 8 }, mainTextSize, mainTextSpacing, textHash);

        for (int i = 0; i < timelineCount; i++)
        {
            float y = listTop + i * lineStep - commitScroll * lineStep;
            if (y < -lineStep || y > height - bottomBarHeight) continue;

            if (i == selected)
                DrawRectangle(10, y, leftWidth - 20, lineStep,
                              rowSelected);
            else if (i == hover)
                DrawRectangle(10, y, leftWidth - 20, lineStep,
                              rowHover);

            if (timeline[i].type == TIMELINE_COMMIT)
            {
                DrawTextEx(font, timeline[i].hash, (Vector2){ 20, y }, mainTextSize, mainTextSpacing, textHash);
                DrawTextEx(font, timeline[i].title, (Vector2){ 96, y }, mainTextSize, mainTextSpacing, textMain);
            }
            else
            {
                DrawTextEx(font, timeline[i].title, (Vector2){ 20, y }, mainTextSize, mainTextSpacing, textMain);
            }
        }
        if (timelineCount == 0)
        {
            DrawTextEx(font, statusHint,
                       (Vector2){ 20, (float)listTop },
                       mainTextSize, mainTextSpacing, textMain);
        }

        DrawRectangle(leftWidth, 0, width - leftWidth, height, bgDiff);

        float dy = listTop - diffScroll * lineStep;
        if (parsedDiff.lineCount == 0)
        {
            DrawTextEx(font, "Select a timeline item to view its diff.",
                       (Vector2){ leftWidth + 12, (float)listTop },
                       mainTextSize, mainTextSpacing, textMain);
        }
        for (int f = 0; f < parsedDiff.fileCount && dy < height - bottomBarHeight; f++)
        {
            Color panelHeader = (f == activePanel) ? (Color){ 45, 62, 76, 255 } : (Color){ 26, 36, 44, 255 };
            DrawRectangle(leftWidth + 8, (int)dy, width - leftWidth - 16, (int)lineStep, panelHeader);
            DrawTextEx(font, panelCollapsed[f] ? "[+]" : "[-]",
                       (Vector2){ (float)leftWidth + 12, dy + 1 }, mainTextSize, mainTextSpacing, textMain);
            DrawTextEx(font, parsedDiff.files[f].path,
                       (Vector2){ (float)leftWidth + 46, dy + 1 }, mainTextSize, mainTextSpacing, textHash);
            dy += lineStep;

            int start = parsedDiff.files[f].lineStart;
            int count = parsedDiff.files[f].lineCount;
            for (int i = 0; !panelCollapsed[f] && i < count && dy < height - bottomBarHeight; i++)
            {
                DiffLine *dl = &parsedDiff.lines[start + i];
                Color c = textMain;
                if (dl->type == '+') c = plusColor;
                else if (dl->type == '-') c = minusColor;
                else if (dl->type == '@') c = textHash;

                DrawTextEx(font, dl->text, (Vector2){ (float)leftWidth + 12, dy }, mainTextSize, mainTextSpacing, c);
                dy += lineStep;
            }
            dy += lineStep;
        }

        DrawRectangle(leftTrackX, leftTrackY, 6, leftTrackH, (Color){ 24, 30, 38, 255 });
        DrawRectangle((int)leftThumbRec.x, (int)leftThumbRec.y, (int)leftThumbRec.width, (int)leftThumbRec.height,
                      dragLeftScrollbar ? (Color){ 120, 144, 162, 255 } : (Color){ 85, 104, 118, 255 });
        DrawRectangle(rightTrackX, rightTrackY, 6, rightTrackH, (Color){ 24, 30, 38, 255 });
        DrawRectangle((int)rightThumbRec.x, (int)rightThumbRec.y, (int)rightThumbRec.width, (int)rightThumbRec.height,
                      dragRightScrollbar ? (Color){ 120, 144, 162, 255 } : (Color){ 85, 104, 118, 255 });

        DrawRectangle(0, height - bottomBarHeight, width, bottomBarHeight, (Color){ 17, 23, 29, 255 });
        DrawLine(0, height - bottomBarHeight, width, height - bottomBarHeight, divider);
        const char *rootText = (repoRoot[0] != 0) ? repoRoot : "(no repo)";
        float infoSize = 14.0f;
        float infoSpacing = 1.0f;
        DrawTextEx(fontSmall,
                   TextFormat("%s | branch: %s | unstaged: %d  staged: %d  untracked: %d",
                              rootText, branchName[0] ? branchName : "(unknown)",
                              unstagedFilesCount, stagedFilesCount, untrackedFilesCount),
                   (Vector2){ 10, (float)height - bottomBarHeight + 6 }, infoSize, infoSpacing, textMain);
        const char *help = "Ctrl+O Open Repo  R Refresh  J/K Files  N/P Hunks  Ctrl+ +/- Zoom";
        int helpW = MeasureTextEx(fontSmall, help, infoSize, infoSpacing).x;
        DrawTextEx(fontSmall, help,
                   (Vector2){ (float)(width - helpW - 10), (float)height - bottomBarHeight + 6 },
                   infoSize, infoSpacing, textHash);

        if (repoInputActive)
        {
            DrawRectangle(0, 0, width, height, (Color){ 0, 0, 0, 160 });
            int boxW = (int)(width * 0.78f);
            if (boxW < 640) boxW = 640;
            if (boxW > width - 40) boxW = width - 40;
            int boxH = 460;
            int boxX = (width - boxW) / 2;
            int boxY = (height - boxH) / 2;

            Rectangle window = { (float)boxX, (float)boxY, (float)boxW, (float)boxH };
            if (GuiWindowBox(window, "Repository Picker")) repoInputActive = false;
            GuiLabel((Rectangle){ (float)boxX + 16, (float)boxY + 44, 72, 24 }, "Look in:");
            if (GuiTextBox((Rectangle){ (float)boxX + 82, (float)boxY + 42, (float)boxW - 318, 28 },
                           repoInputPath, (int)sizeof(repoInputPath), repoInputEditMode))
            {
                repoInputEditMode = !repoInputEditMode;
                if (!repoInputEditMode) RefreshPickerItems();
            }
            if (GuiButton((Rectangle){ (float)boxX + boxW - 226, (float)boxY + 42, 64, 28 }, "Up"))
            {
                PickerGoParent();
            }
            if (GuiButton((Rectangle){ (float)boxX + boxW - 154, (float)boxY + 42, 64, 28 }, "Go"))
            {
                PickerOpenSelected();
            }
            if (GuiButton((Rectangle){ (float)boxX + boxW - 82, (float)boxY + 42, 64, 28 }, "R"))
            {
                RefreshPickerItems();
            }
            GuiListViewEx((Rectangle){ (float)boxX + 16, (float)boxY + 80, (float)boxW - 32, (float)boxH - 162 },
                          pickerItemPtrs, pickerItemCount, &pickerScrollIndex, &pickerActive, &pickerFocus);
            GuiLabel((Rectangle){ (float)boxX + 16, (float)boxY + boxH - 70, 80, 24 }, "Folder:");
            GuiTextBox((Rectangle){ (float)boxX + 82, (float)boxY + boxH - 72, (float)boxW - 318, 28 },
                       repoInputPath, (int)sizeof(repoInputPath), false);
            if (GuiButton((Rectangle){ (float)boxX + boxW - 226, (float)boxY + boxH - 72, 96, 28 }, "Open"))
            {
                if (repoInputPath[0] == 0)
                {
                    CopyBounded(repoInputHint, sizeof(repoInputHint), "Path cannot be empty");
                }
                else if (ReloadFromRepoPath(repoInputPath))
                {
                    repoInputActive = false;
                }
                else
                {
                    CopyBounded(repoInputHint, sizeof(repoInputHint), "Not a git repository");
                }
            }
            if (GuiButton((Rectangle){ (float)boxX + boxW - 122, (float)boxY + boxH - 72, 96, 28 }, "Cancel"))
            {
                repoInputActive = false;
            }
            if (repoInputHint[0] != 0)
            {
                GuiLabel((Rectangle){ (float)boxX + 16, (float)boxY + boxH - 42, (float)boxW - 32, 24 }, repoInputHint);
            }
        }

        EndDrawing();
    }

    if (fontSmallOwned) UnloadFont(fontSmall);
    if (fontOwned) UnloadFont(font);
    CloseWindow();
    return 0;
}
