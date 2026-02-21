/********************************************************************
 Git Commit + Diff Visualizer
********************************************************************/

#include "raylib.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef Clamp
#define Clamp(value, min, max) \
    ((value) < (min) ? (min) : ((value) > (max) ? (max) : (value)))
#endif

#define MAX_FILES      512
#define MAX_LINE       1024
#define MAX_DIFF_LINES 20000
#define MAX_HUNKS      2048

typedef struct {
    char type;
    int oldLine;
    int newLine;
    char text[MAX_LINE];
} DiffLine;

typedef struct {
    int oldStart;
    int oldCount;
    int newStart;
    int newCount;
    int lineStart;
    int lineCount;
    char header[128];
} DiffHunk;

typedef struct {
    char indexStatus;
    char worktreeStatus;
    char path[512];
    char oldPath[512];
    bool isRenamed;
    bool isUntracked;
} ChangedFile;

typedef struct {
    ChangedFile files[MAX_FILES];
    int fileCount;
} RepoStatus;

typedef struct {
    DiffHunk hunks[MAX_HUNKS];
    int hunkCount;
    DiffLine lines[MAX_DIFF_LINES];
    int lineCount;
} ParsedDiff;

static RepoStatus repoStatus;
static ParsedDiff parsedDiff;

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

static void ParseHunkHeader(const char *line, DiffHunk *hunk)
{
    int oldStart = 0, oldCount = 1, newStart = 0, newCount = 1;
    if (sscanf(line, "@@ -%d,%d +%d,%d @@", &oldStart, &oldCount, &newStart, &newCount) == 4)
    {
        hunk->oldStart = oldStart;
        hunk->oldCount = oldCount;
        hunk->newStart = newStart;
        hunk->newCount = newCount;
        return;
    }
    if (sscanf(line, "@@ -%d +%d,%d @@", &oldStart, &newStart, &newCount) == 3)
    {
        hunk->oldStart = oldStart;
        hunk->oldCount = 1;
        hunk->newStart = newStart;
        hunk->newCount = newCount;
        return;
    }
    if (sscanf(line, "@@ -%d,%d +%d @@", &oldStart, &oldCount, &newStart) == 3)
    {
        hunk->oldStart = oldStart;
        hunk->oldCount = oldCount;
        hunk->newStart = newStart;
        hunk->newCount = 1;
        return;
    }
    if (sscanf(line, "@@ -%d +%d @@", &oldStart, &newStart) == 2)
    {
        hunk->oldStart = oldStart;
        hunk->oldCount = 1;
        hunk->newStart = newStart;
        hunk->newCount = 1;
        return;
    }
}

static void ParseStatusLine(const char *line, ChangedFile *out)
{
    memset(out, 0, sizeof(*out));

    if (strlen(line) < 4) return;

    out->indexStatus = line[0];
    out->worktreeStatus = line[1];
    out->isUntracked = (line[0] == '?' && line[1] == '?');

    const char *pathStart = line + 3;
    CopyBounded(out->path, sizeof(out->path), pathStart);
    TrimTrailingNewline(out->path);

    char *arrow = strstr(out->path, " -> ");
    if (arrow)
    {
        out->isRenamed = true;
        *arrow = 0;
        CopyBounded(out->oldPath, sizeof(out->oldPath), out->path);
        CopyBounded(out->path, sizeof(out->path), arrow + 4);
    }
}

static void LoadRepoStatus(void)
{
    FILE *fp = popen("git status --porcelain=v1", "r");
    if (!fp) return;

    repoStatus.fileCount = 0;
    char line[MAX_LINE];

    while (fgets(line, sizeof(line), fp) && repoStatus.fileCount < MAX_FILES)
    {
        ParseStatusLine(line, &repoStatus.files[repoStatus.fileCount]);
        if (repoStatus.files[repoStatus.fileCount].path[0] != 0)
            repoStatus.fileCount++;
    }
    pclose(fp);
}

/* ------------------------------------------------------------ */

static void LoadDiffForFile(int index)
{
    if (index < 0 || index >= repoStatus.fileCount) return;

    char cmd[640];
    snprintf(cmd, sizeof(cmd),
             "git diff --no-color -- \"%s\"",
             repoStatus.files[index].path);

    FILE *fp = popen(cmd, "r");
    if (!fp) return;

    parsedDiff.hunkCount = 0;
    parsedDiff.lineCount = 0;
    int currentHunk = -1;
    int oldLine = 0;
    int newLine = 0;
    char line[MAX_LINE];

    while (fgets(line, sizeof(line), fp) && parsedDiff.lineCount < MAX_DIFF_LINES)
    {
        TrimTrailingNewline(line);

        if (strncmp(line, "@@ ", 3) == 0 && parsedDiff.hunkCount < MAX_HUNKS)
        {
            DiffHunk *h = &parsedDiff.hunks[parsedDiff.hunkCount++];
            memset(h, 0, sizeof(*h));
            CopyBounded(h->header, sizeof(h->header), line);
            ParseHunkHeader(line, h);
            h->lineStart = parsedDiff.lineCount;
            currentHunk = parsedDiff.hunkCount - 1;
            oldLine = h->oldStart;
            newLine = h->newStart;
        }

        DiffLine *dl = &parsedDiff.lines[parsedDiff.lineCount++];
        memset(dl, 0, sizeof(*dl));
        dl->type = line[0] ? line[0] : ' ';
        CopyBounded(dl->text, sizeof(dl->text), line);

        if (currentHunk >= 0)
        {
            if (dl->type == ' ')
            {
                dl->oldLine = oldLine++;
                dl->newLine = newLine++;
            }
            else if (dl->type == '-')
            {
                dl->oldLine = oldLine++;
            }
            else if (dl->type == '+')
            {
                dl->newLine = newLine++;
            }
            parsedDiff.hunks[currentHunk].lineCount++;
        }
    }
    pclose(fp);
}

/* ------------------------------------------------------------ */

int main(void)
{
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(1200, 800, "gitviz");

    font = LoadFontEx("assets/fonts/UbuntuMono-R.ttf", 32, 0, 0);
    if (font.texture.id == 0)
    {
        TraceLog(LOG_WARNING, "Failed to load Ubuntu Mono, using default font");
        font = GetFontDefault();
    }
    SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);

    LoadRepoStatus();
    if (repoStatus.fileCount > 0)
        LoadDiffForFile(0);

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

        if (IsKeyPressed(KEY_DOWN) && selected < repoStatus.fileCount - 1)
        {
            selected++;
            LoadDiffForFile(selected);
        }
        if (IsKeyPressed(KEY_UP) && selected > 0)
        {
            selected--;
            LoadDiffForFile(selected);
        }
        if (IsKeyPressed(KEY_R))
        {
            LoadRepoStatus();
            if (selected >= repoStatus.fileCount)
                selected = repoStatus.fileCount > 0 ? repoStatus.fileCount - 1 : 0;
            if (repoStatus.fileCount > 0)
                LoadDiffForFile(selected);
            else
                parsedDiff.lineCount = parsedDiff.hunkCount = 0;
        }

        /* ---------- Mouse ---------- */

        Vector2 mouse = GetMousePosition();
        float wheel = GetMouseWheelMove();

        int leftWidth = 430;
        int headerHeight = 34;
        int listTop = headerHeight + 10;

        if (mouse.x < leftWidth)
            commitScroll -= (int)(wheel * 3);
        else
            diffScroll -= (int)(wheel * 3);

        if (commitScroll < 0) commitScroll = 0;
        if (diffScroll < 0) diffScroll = 0;

        hover = -1;

        int y0 = listTop - commitScroll * lineStep;
        for (int i = 0; i < repoStatus.fileCount; i++)
        {
            Rectangle r = {
                10, y0 + i * lineStep,
                leftWidth - 20, lineStep
            };

            if (CheckCollisionPointRec(mouse, r))
            {
                hover = i;
                if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
                {
                    selected = i;
                    diffScroll = 0;
                    LoadDiffForFile(i);
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
        DrawTextEx(font, "CHANGED FILES", (Vector2){ 12, 8 }, fontSize, 1, textHash);
        DrawLineEx((Vector2){ leftWidth, 0 },
                   (Vector2){ leftWidth, height }, 2.0f, divider);
        DrawRectangle(leftWidth, 0, width - leftWidth, headerHeight, (Color){ 20, 28, 34, 255 });
        if (repoStatus.fileCount > 0)
            DrawTextEx(font, TextFormat("DIFF %s", repoStatus.files[selected].path),
                       (Vector2){ leftWidth + 12, 8 }, fontSize, 1, textHash);
        else
            DrawTextEx(font, "DIFF", (Vector2){ leftWidth + 12, 8 }, fontSize, 1, textHash);

        for (int i = 0; i < repoStatus.fileCount; i++)
        {
            float y = listTop + i * lineStep - commitScroll * lineStep;
            if (y < -lineStep || y > height) continue;

            if (i == selected)
                DrawRectangle(10, y, leftWidth - 20, lineStep,
                              rowSelected);
            else if (i == hover)
                DrawRectangle(10, y, leftWidth - 20, lineStep,
                              rowHover);

            char statusText[4];
            snprintf(statusText, sizeof(statusText), "%c%c",
                     repoStatus.files[i].indexStatus,
                     repoStatus.files[i].worktreeStatus);
            DrawTextEx(font, statusText,
                       (Vector2){ 20, y },
                       fontSize, 1, textHash);

            DrawTextEx(font, repoStatus.files[i].path,
                       (Vector2){ 74, y },
                       fontSize, 1, textMain);
        }

        DrawRectangle(leftWidth, 0, width - leftWidth, height, bgDiff);

        float dy = listTop - diffScroll * lineStep;
        for (int i = 0; i < parsedDiff.lineCount && dy < height; i++)
        {
            Color c = textMain;
            if (parsedDiff.lines[i].type == '+') c = plusColor;
            else if (parsedDiff.lines[i].type == '-') c = minusColor;
            else if (parsedDiff.lines[i].type == '@') c = textHash;

            DrawTextEx(font, parsedDiff.lines[i].text,
                       (Vector2){ leftWidth + 10, dy },
                       fontSize, 1, c);

            dy += lineStep;
        }

        EndDrawing();
    }

    UnloadFont(font);
    CloseWindow();
    return 0;
}
