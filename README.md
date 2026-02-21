## gitviz

A lightweight local Git visualizer.

Current UI:
- Left pane timeline: `Unstaged`, `Staged`, and recent commits
- Right pane grouped file diff panels with collapse/expand and hunk/file jumps
- Bottom status bar with repo/branch/change counts and key hints

Quick start:
- Build: `make`
- Run: `make run REPO=/path/to/repo`
- Help: `./build/gitviz --help`
- Install (binary + app launcher + fonts): `sudo make install`
- Uninstall: `sudo make uninstall`

Architecture notes:
- `docs/architecture.md`
