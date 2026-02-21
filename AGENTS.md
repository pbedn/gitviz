# AGENTS.md

## Project intent

`gitviz` is a simple, local Git visualizer built in C with raylib (and raygui for small UI pieces).
The code should stay straightforward and easy to read.

## Design choices

- Keep architecture practical and explicit:
  - clear data structs,
  - clear input/update/render flow,
  - no hidden magic behavior.
- Prefer predictable behavior over clever optimization.
- Keep UI interactions discoverable and stable.
- Use `git -C <repoRoot>` for git commands so runtime is independent of current working directory.

## C style rules

- Write plain C11.
- Prefer simple control flow and small helper functions.
- Avoid advanced tricks/macros unless absolutely necessary.
- Avoid dense one-liners when they reduce readability.
- Use descriptive names for functions/variables.
- Keep constants explicit (`#define` values with meaningful names).
- Validate bounds and indices defensively.

## Function comment rules

Every non-trivial function should have a short comment above it describing:

1. What it does.
2. Inputs/outputs or side effects.
3. Important assumptions.

Keep comments short and practical.

Example format:

```c
// Build timeline items from git state.
// Side effects: updates timeline[], timelineCount, branch/counter fields.
// Assumes repoRoot is valid and non-empty.
static void LoadTimeline(void) { ... }
```

## Line comment rules

- Add line comments only where logic is not obvious.
- Use comments for intent, not for restating code.
- Good use cases:
  - tricky math,
  - state synchronization,
  - parser edge cases,
  - UI interaction subtleties.

## raylib/raygui style rules

- Keep rendering path simple:
  - input/update first,
  - then drawing,
  - then modal overlays.
- Keep colors and spacing centralized when possible.
- Avoid heavy visual complexity; prefer readable contrast and clear hierarchy.
- Keep keyboard and mouse interactions consistent across panes.
- UI behavior should remain responsive under normal repo sizes.

## Testing and safety

- Prefer adding tests for parser/helper behavior when possible.
- After any behavior change, update existing tests (or add new ones) so tests reflect current behavior.
- Before every commit, run the test suite (`make test`) and fix failures first.
- For UI changes, preserve existing key workflows (`R`, `Ctrl+O`, navigation keys, scrolling, splitter).
- Do not introduce destructive git operations.

## Documentation

- Update docs when behavior or architecture meaningfully changes:
  - `README.md` for user-facing changes,
  - `docs/architecture.md` for structural/design changes.
