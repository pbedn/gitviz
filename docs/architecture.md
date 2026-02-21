# gitviz Architecture and Design Choices

## 1. Purpose and Product Direction

`gitviz` is a single-binary desktop UI for local git repository inspection, focused on:

- Fast visual understanding of repository changes.
- A workflow similar to Sublime Merge:
  - timeline-like selection on the left (`Unstaged`, `Staged`, recent commits),
  - grouped diffs on the right (file panels with hunk-level navigation).
- Zero runtime dependencies beyond system libs required by raylib.

The project currently prioritizes:

- Readability and interaction speed.
- Local-first behavior (no network, no daemon).
- Portable startup behavior (`gitviz [repo-path]`, not tied to process CWD).

## 2. Top-Level Architecture

The application is intentionally built as a single translation unit (`gitviz.c`) with explicit subsystems:

1. Repository resolution and git command execution.
2. Data model construction (timeline + parsed diff + UI state).
3. Input/event handling (keyboard, mouse, scrollbars, splitter, modal dialog).
4. Rendering (timeline pane, diff pane, bottom status pane, modal picker).

This monolithic file keeps iteration fast for a UI prototype. The data model is structured so code can later be split into modules without changing behavior.

## 3. Core Data Model

### Timeline

`TimelineItem` represents one left-pane row:

- `TIMELINE_UNSTAGED`
- `TIMELINE_STAGED`
- `TIMELINE_COMMIT`

Each item includes:

- short hash (for commits),
- title text (commit subject or synthetic labels like `Unstaged Files (N)`).

### Diff Panels

Right pane state is represented by `ParsedDiff`:

- `DiffLine[]`: flattened raw diff lines.
- `DiffFilePanel[]`: per-file section boundaries:
  - file path,
  - start index into `DiffLine[]`,
  - number of lines for that file.
- `DiffHunkRef[]`: hunk anchors for quick navigation:
  - file index,
  - line index within file.

This structure enables:

- collapse/expand per file panel,
- efficient panel header rendering,
- jump between files/hunks without reparsing.

### UI State

Persistent UI state is stored explicitly:

- selection (`selected` timeline row),
- scroll offsets (`commitScroll`, `diffScroll`),
- active panel/hunk (`activePanel`, `activeHunk`),
- panel collapse flags (`panelCollapsed[]`),
- pane geometry (`leftPaneWidth`),
- drag state flags for splitter and scrollbars.

## 4. Git Integration Strategy

### Process Model

Git interactions use shell commands executed via `popen()` and parsed line-by-line.

All commands are repository-root explicit:

- `git -C '<repoRoot>' ...`

This removes dependence on launch location and supports running the binary from any directory.

### Timeline Loading

`LoadTimeline()` collects:

- unstaged file count (`git diff --name-only`),
- staged file count (`git diff --cached --name-only`),
- untracked count (`git ls-files --others --exclude-standard`),
- branch name (`git rev-parse --abbrev-ref HEAD`),
- recent commits (`git log --oneline`).

### Diff Loading

`LoadDiffForSelection()` chooses source by timeline type:

- unstaged: `git diff --no-color`
- staged: `git diff --cached --no-color`
- commit: `git show --no-color --pretty=format: <hash>`

`ParseDiffStream()` segments diff by `diff --git` boundaries, creating file panels and hunk references.

## 5. Rendering Architecture

The draw path is deterministic per frame:

1. Left pane (timeline + selection/hover + scrollbar).
2. Right pane (file panels + collapsible headers + line coloring + scrollbar).
3. Splitter and bottom status pane.
4. Modal repo picker overlay (raygui) when active.

### Color and Text

Semantic color mapping:

- panel headers and selected rows: cooler accent tones,
- added lines: green,
- removed lines: red,
- hunk headers and metadata: hash/accent color.

Fonts are loaded from `assets/fonts` only. If unavailable, fallback is raylib default font.

## 6. Input and Interaction Model

### Timeline Navigation

- `Up/Down`: move left selection.
- click row: select timeline item.
- `R`: refresh timeline and keep closest selection.

### Diff Navigation

- `J/K`: next/previous file panel.
- `N/P`: next/previous hunk.
- file-panel header click: collapse/expand panel.

### Scrolling

- wheel scroll routes to pane under cursor.
- custom scrollbars support:
  - track click jump,
  - thumb drag,
  - enlarged hit areas for better usability.

### Layout

- draggable vertical splitter resizes timeline pane.
- width is clamped to preserve minimum usable space in both panes.

### Repository Selection

- CLI: `gitviz [repo-path]`, `-r/--repo`.
- In-app: `Ctrl+O` opens raygui repository picker modal.

## 7. Bottom Status Pane Design

The bottom pane is intentionally compact and always visible.

Displayed:

- repository path,
- current branch,
- unstaged/staged/untracked counts,
- shortcut hints.

It acts as both context and discoverability surface without consuming primary diff area.

## 8. Why These Design Choices

### 1) Structured diff panels over plain text stream

Reason:

- enables collapse/expand,
- supports hunk/file jumps,
- scales better as interactions increase.

### 2) `git -C` everywhere

Reason:

- deterministic behavior regardless of working directory,
- easier integration with external launchers/editors.

### 3) Explicit mutable UI state

Reason:

- immediate-mode rendering needs stable state,
- keeps input-to-render mapping predictable and debuggable.

### 4) Custom scrollbars and splitter

Reason:

- better fit for dual-pane diff UX than generic controls,
- direct control over hit areas and behavior.

### 5) raygui modal for repo selection

Reason:

- consistent look with in-app interactions,
- no extra native dialog dependency.

## 9. Performance and Limits

Current limits are compile-time constants:

- max timeline items,
- max diff files,
- max diff lines,
- max hunk references.

Diff parsing is single-pass and linear in output size.

Known current tradeoffs:

- every selection triggers a fresh git command,
- no incremental diff cache,
- no background worker thread.

These are acceptable for small/medium repositories and keep behavior simple and deterministic.

## 10. Failure Handling

Failure philosophy:

- degrade gracefully,
- keep UI responsive,
- communicate state in bottom/status text.

Examples:

- invalid repo path -> non-crashing error hint,
- empty diffs -> explicit empty-state messaging,
- missing fonts -> default raylib font fallback.

## 11. Planned Evolution Path

Near-term architecture-safe additions:

1. Persisted UI prefs (pane width, font size, key mode).
2. File/hunk search.
3. Optional diff cache keyed by selection.
4. Better staged/unstaged mixed-state visualization.
5. Multi-file action commands (stage/unstage from UI).

Longer-term structural refactor:

- split into modules:
  - `repo_git.*`,
  - `diff_model.*`,
  - `ui_timeline.*`,
  - `ui_diff.*`,
  - `ui_picker.*`.

Current data model already supports this extraction with minimal behavior changes.

## 12. Build and Runtime Contract

Build:

- `make`

Run:

- `./build/gitviz [repo-path]`

Assumptions:

- git executable is available,
- repository path is local and readable,
- raylib static library and link dependencies are present.
