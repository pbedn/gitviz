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
    DiffLine lines[MAX_DIFF_LINES];
    int lineCount;
    DiffFilePanel files[MAX_DIFF_FILES];
    int fileCount;
} ParsedDiff;

static TimelineItem timeline[MAX_TIMELINE];
static int timelineCount = 0;
static ParsedDiff parsedDiff;
static char statusHint[128];
static char repoRoot[512];
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

static Font font;

/* ------------------------------------------------------------ */

static void TrimTrailingNewline(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
    {
        s[n - 1] = 0;
        n--;
    }
}

static void CopyBounded(char *dst, size_t dstSize, const char *src)
{
    if (dstSize == 0) return;
    size_t n = strlen(src);
    if (n >= dstSize) n = dstSize - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

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

static bool IsDirectoryPath(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0) && S_ISDIR(st.st_mode);
}

static void JoinPath(char *dst, size_t dstSize, const char *base, const char *name)
{
    if ((name[0] == '/') || (base[0] == 0))
    {
        CopyBounded(dst, dstSize, name);
        return;
    }
    snprintf(dst, dstSize, "%s/%s", base, name);
}

static void TrimTrailingSlash(char *path)
{
    size_t n = strlen(path);
    while ((n > 1) && (path[n - 1] == '/'))
    {
        path[n - 1] = 0;
        n--;
    }
}

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

static int CompareStrings(const void *a, const void *b)
{
    const char *sa = (const char *)a;
    const char *sb = (const char *)b;
    return strcmp(sa, sb);
}

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

static void PickerGoParent(void)
{
    GoParentPath(repoInputPath, sizeof(repoInputPath));
    RefreshPickerItems();
}

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

static int AddDiffPanel(const char *path)
{
    if (parsedDiff.fileCount >= MAX_DIFF_FILES) return -1;
    DiffFilePanel *p = &parsedDiff.files[parsedDiff.fileCount++];
    memset(p, 0, sizeof(*p));
    CopyBounded(p->path, sizeof(p->path), path);
    p->lineStart = parsedDiff.lineCount;
    return parsedDiff.fileCount - 1;
}

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

static void ParseDiffStream(FILE *fp)
{
    parsedDiff.lineCount = 0;
    parsedDiff.fileCount = 0;

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

        DiffLine *dl = &parsedDiff.lines[parsedDiff.lineCount++];
        dl->type = line[0] ? line[0] : ' ';
        CopyBounded(dl->text, sizeof(dl->text), line);
        parsedDiff.files[currentPanel].lineCount++;
    }
}

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

    int unstagedCount = 0;
    int stagedCount = 0;
    snprintf(cmd, sizeof(cmd), "git -C %s diff --name-only", qRepo);
    unstagedCount = CountLinesInCommand(cmd);
    snprintf(cmd, sizeof(cmd), "git -C %s diff --cached --name-only", qRepo);
    stagedCount = CountLinesInCommand(cmd);

    timeline[timelineCount].type = TIMELINE_UNSTAGED;
    timeline[timelineCount].hash[0] = 0;
    snprintf(timeline[timelineCount].title, sizeof(timeline[timelineCount].title),
             "Unstaged Files (%d)", unstagedCount);
    timelineCount++;

    timeline[timelineCount].type = TIMELINE_STAGED;
    timeline[timelineCount].hash[0] = 0;
    snprintf(timeline[timelineCount].title, sizeof(timeline[timelineCount].title),
             "Staged Files (%d)", stagedCount);
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

static void LoadDiffForSelection(int index)
{
    parsedDiff.lineCount = 0;
    parsedDiff.fileCount = 0;

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
}

/* ------------------------------------------------------------ */

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
        TraceLog(LOG_WARNING, "Failed to load Ubuntu Mono, using default font");
        font = GetFontDefault();
    }
    SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);

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

        lineStep = fontSize * 1.35f;

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

        /* ---------- Mouse ---------- */

        Vector2 mouse = GetMousePosition();
        float wheel = GetMouseWheelMove();

        int leftWidth = 430;
        int headerHeight = 34;
        int listTop = headerHeight + 10;

        if (!repoInputActive && mouse.x < leftWidth)
            commitScroll -= (int)(wheel * 3);
        else if (!repoInputActive)
            diffScroll -= (int)(wheel * 3);

        if (commitScroll < 0) commitScroll = 0;
        if (diffScroll < 0) diffScroll = 0;

        hover = -1;

        int y0 = listTop - commitScroll * lineStep;
        for (int i = 0; i < timelineCount; i++)
        {
            Rectangle r = {
                10, y0 + i * lineStep,
                leftWidth - 20, lineStep
            };

            if (!repoInputActive && CheckCollisionPointRec(mouse, r))
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

        int width  = GetScreenWidth();
        int height = GetScreenHeight();

        DrawRectangle(0, 0, leftWidth, height, bgSidebar);
        DrawRectangle(0, 0, leftWidth, headerHeight, (Color){ 34, 45, 56, 255 });
        DrawTextEx(font, "TIMELINE", (Vector2){ 12, 8 }, fontSize, 1, textHash);
        DrawLineEx((Vector2){ leftWidth, 0 },
                   (Vector2){ leftWidth, height }, 2.0f, divider);
        DrawRectangle(leftWidth, 0, width - leftWidth, headerHeight, (Color){ 20, 28, 34, 255 });
        if (timelineCount > 0)
        {
            if (timeline[selected].type == TIMELINE_COMMIT)
                DrawTextEx(font, TextFormat("DIFF %s  %s", timeline[selected].hash, timeline[selected].title),
                           (Vector2){ leftWidth + 12, 8 }, fontSize, 1, textHash);
            else
                DrawTextEx(font, TextFormat("DIFF %s", timeline[selected].title),
                           (Vector2){ leftWidth + 12, 8 }, fontSize, 1, textHash);
        }
        else
            DrawTextEx(font, "DIFF", (Vector2){ leftWidth + 12, 8 }, fontSize, 1, textHash);

        for (int i = 0; i < timelineCount; i++)
        {
            float y = listTop + i * lineStep - commitScroll * lineStep;
            if (y < -lineStep || y > height) continue;

            if (i == selected)
                DrawRectangle(10, y, leftWidth - 20, lineStep,
                              rowSelected);
            else if (i == hover)
                DrawRectangle(10, y, leftWidth - 20, lineStep,
                              rowHover);

            if (timeline[i].type == TIMELINE_COMMIT)
            {
                DrawTextEx(font, timeline[i].hash, (Vector2){ 20, y }, fontSize, 1, textHash);
                DrawTextEx(font, timeline[i].title, (Vector2){ 96, y }, fontSize, 1, textMain);
            }
            else
            {
                DrawTextEx(font, timeline[i].title, (Vector2){ 20, y }, fontSize, 1, textMain);
            }
        }
        if (timelineCount == 0)
        {
            DrawTextEx(font, statusHint,
                       (Vector2){ 20, (float)listTop },
                       fontSize, 1, textMain);
        }

        DrawRectangle(leftWidth, 0, width - leftWidth, height, bgDiff);

        float dy = listTop - diffScroll * lineStep;
        if (parsedDiff.lineCount == 0)
        {
            DrawTextEx(font, "Select a timeline item to view its diff.",
                       (Vector2){ leftWidth + 12, (float)listTop },
                       fontSize, 1, textMain);
        }
        for (int f = 0; f < parsedDiff.fileCount && dy < height; f++)
        {
            DrawRectangle(leftWidth + 8, (int)dy, width - leftWidth - 16, (int)(lineStep + 8), (Color){ 26, 36, 44, 255 });
            DrawTextEx(font, parsedDiff.files[f].path,
                       (Vector2){ (float)leftWidth + 14, dy + 4 }, fontSize, 1, textHash);
            dy += lineStep + 10;

            int start = parsedDiff.files[f].lineStart;
            int count = parsedDiff.files[f].lineCount;
            for (int i = 0; i < count && dy < height; i++)
            {
                DiffLine *dl = &parsedDiff.lines[start + i];
                Color c = textMain;
                if (dl->type == '+') c = plusColor;
                else if (dl->type == '-') c = minusColor;
                else if (dl->type == '@') c = textHash;

                DrawTextEx(font, dl->text, (Vector2){ (float)leftWidth + 12, dy }, fontSize, 1, c);
                dy += lineStep;
            }
            dy += 8;
        }

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

    UnloadFont(font);
    CloseWindow();
    return 0;
}
