# Files 0.1.38 — Correct legibility and initial location

This patch release responds to observed defects in 0.1.37:
- File Manager opens at the filesystem root (`/`), not the login home directory.
- The window and all child controls use one scoped per-window Common Day/Night palette.
- Native GTK header controls and breadcrumb labels have explicit accessible foreground colours.
- The full GTK4 ColumnView node hierarchy is styled, eliminating the white-sidebar/dark-file-row mismatch.
- File rows, selection, tabs, sidebar, status line and context menus share consistent semantic colour roles.
- Common's cyan/info role provides a restrained accent for selected locations and active tabs; this is a file manager, not a dashboard.
- The new theme-presentation regression test checks root startup, Day/Night text colours and independent multi-window theme isolation.

No bespoke calendar, file operation or filesystem transfer logic was introduced.
These automated checks do not replace visual confirmation on the actual GTK theme and desktop compositor.
