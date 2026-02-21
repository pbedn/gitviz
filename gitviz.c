/********************************************************************
 Minimal Git Commit + Diff Visualizer (single-file MVP)

 Features:
 - Reads commits using git log
 - Displays commit list
 - Shows git show diff for selected commit

 Limitations:
 - No real graph yet (list only)
 - Fixed buffers
 - Blocking git calls
********************************************************************/

#include "raylib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_COMMITS 256
#define MAX_LINE    1024
#define MAX_DIFF    200000

typedef struct {
    char hash[16];
    char subject[256];
} Commit;

static Commit commits[MAX_COMMITS];
static int commitCount = 0;
static int selected = 0;

static char diffText[MAX_DIFF];

/* ------------------------------------------------------------ */

static void LoadCommits(void)
{
    FILE *fp = popen(
        "git log --oneline --max-count=256", "r"
    );
    if (!fp) return;

    commitCount = 0;

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), fp) && commitCount < MAX_COMMITS)
    {
        char *space = strchr(line, ' ');
        if (!space) continue;

        *space = 0;
        strncpy(commits[commitCount].hash, line, 15);
        strncpy(commits[commitCount].subject, space + 1, 255);

        // strip newline
        commits[commitCount].subject[
            strcspn(commits[commitCount].subject, "\n")
        ] = 0;

        commitCount++;
    }

    pclose(fp);
}

/* ------------------------------------------------------------ */

static void LoadDiffForCommit(int index)
{
    if (index < 0 || index >= commitCount) return;

    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "git show %s --no-color",
             commits[index].hash);

    FILE *fp = popen(cmd, "r");
    if (!fp) return;

    diffText[0] = 0;
    char line[MAX_LINE];

    size_t offset = 0;
    while (fgets(line, sizeof(line), fp))
    {
        size_t len = strlen(line);
        if (offset + len >= MAX_DIFF) break;

        memcpy(diffText + offset, line, len);
        offset += len;
    }

    diffText[offset] = 0;
    pclose(fp);
}

/* ------------------------------------------------------------ */

int main(void)
{
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(1200, 800, "gitviz (MVP)");

    SetTargetFPS(60);

    LoadCommits();
    if (commitCount > 0)
        LoadDiffForCommit(0);

    while (!WindowShouldClose())
    {
        if (IsKeyPressed(KEY_DOWN) && selected < commitCount - 1)
        {
            selected++;
            LoadDiffForCommit(selected);
        }
        if (IsKeyPressed(KEY_UP) && selected > 0)
        {
            selected--;
            LoadDiffForCommit(selected);
        }

        BeginDrawing();
        ClearBackground((Color){ 30, 30, 30, 255 });

        int width  = GetScreenWidth();
        int height = GetScreenHeight();

        int leftWidth = 420;

        /* Commit list */
        DrawRectangle(0, 0, leftWidth, height, (Color){ 40, 40, 40, 255 });

        int y = 20;
        for (int i = 0; i < commitCount; i++)
        {
            Color col = (i == selected)
                ? (Color){ 80, 80, 120, 255 }
                : (Color){ 200, 200, 200, 255 };

            if (i == selected)
                DrawRectangle(10, y - 2, leftWidth - 20, 20, col);

            DrawText(commits[i].hash, 20, y, 12, BLACK);
            DrawText(commits[i].subject, 90, y, 12, BLACK);

            y += 20;
            if (y > height - 20) break;
        }

        /* Diff view */
        DrawRectangle(leftWidth, 0, width - leftWidth, height,
                      (Color){ 20, 20, 20, 255 });

        int dx = leftWidth + 10;
        int dy = 10;

        const char *p = diffText;
        char line[MAX_LINE];

        while (*p && dy < height - 20)
        {
            int i = 0;
            while (*p && *p != '\n' && i < MAX_LINE - 1)
                line[i++] = *p++;

            if (*p == '\n') p++;
            line[i] = 0;

            Color c = RAYWHITE;
            if (line[0] == '+') c = GREEN;
            else if (line[0] == '-') c = RED;

            DrawText(line, dx, dy, 12, c);
            dy += 14;
        }

        EndDrawing();
    }

    CloseWindow();
    return 0;
}
