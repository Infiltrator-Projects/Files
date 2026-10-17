# Files 0.1.37 — Everyday capabilities and coherent chrome

15 deliberately bounded improvements mapped to the feature catalogue:

1. Multiple independent Files windows (Ctrl+N).
2. Open selected directory in a new Files window.
3. Duplicate and reorder tabs within a window (Ctrl+Shift+D; Ctrl+Shift+PageUp/PageDown).
4. Reopen the most recently closed tab (Ctrl+Shift+T, bounded session history).
5. Navigate to parent with the previous child folder reselected when available.
6. Resize and reorder Detail/List columns using native GTK column affordances.
7. Remember the default view choice in Files-owned XDG preferences.
8. Optional folders-first presentation sorting.
9. Natural numeric filename ordering (file2 before file10) with a deterministic tie-break.
10. Show/hide dotfiles (Ctrl+H), including persistence of that preference.
11. Rubber-band multi-selection in the native List/Icon/Compact views.
12. Read-only Properties expanded with allocated size, POSIX permissions, and
    numeric owner/group identifiers when exposed by the provider.
13. Copy a selected local path or URI as plain text; Ctrl+Shift+C copies the path.
14. Hide/show the sidebar (Ctrl+B), remembering the choice.
15. Conventional shortcuts: Alt+Left/Right/Up/Home and Ctrl+1/2/3 for views.

The UI pass removes the three large textual view selectors from the title bar.
The header now concentrates on navigation, breadcrumbs and the single overflow
menu. View selection moves to the quiet status strip. Tabs have compact
selected-state treatments, sidebar sections use the Common palette, selection
uses high-contrast styling, and empty folders receive a meaningful empty state.

File operations still travel through the same validated controllers/engines.
Clipboard ownership changes now invalidate internal cut/copy selections so
a stale move cannot execute after a path-text or external clipboard replacement.
The filter and sorter maintain state independently of window object lifetimes;
preferences load without creating directories or disrupting Common's temporal
policy watcher.

Scope not yet claimed: move tabs across windows, restore tabs after application
restart, hidden-file policy from system-wide settings, editing ownership/modes,
per-folder view settings, safe drag-drop move, richer thumbnails, and full UI
visual/accessibility qualification on multiple actual desktops. Within-window
tab reordering is keyboard-based, not drag-reorder.

The canonical executable is /Programs/files. Build and test via the hosted
Verify workflow before the release publisher uploads the exact tested Debian
and native-installer artifacts.
