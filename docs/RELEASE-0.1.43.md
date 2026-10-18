# Files 0.1.43 — Hamburger menu and GTK clipboard monitoring reliability

A focused bug-fix release for the **File actions** (hamburger) menu in Files 0.1.42:

- Constrain the main menu's action list to a 340-pixel high GTK scrolling area, instead of allowing a tall unbounded native popover to overflow a laptop display.
- Keep the menu's Copy, Cut and Paste actions accessible outside that scrolled area, while preserving the existing operation controllers and keyboard shortcuts.
- Add a headless GTK regression that **opens and closes** the actual menu, checks it is mapped and sized appropriately, and asserts that important actions remain available.
- Quiesce `GtkDirectoryList` live change monitoring while an owned clipboard batch transfer changes the filesystem, then resume monitoring and reload the current listing on completion. Do not restart monitoring during window teardown; this avoids racing GTK's asynchronous GIO monitor callback against disposed window state.
- Reuse the existing journalled batch transfer engine, verified collision policy and safe cleanup; no copy/move implementation was duplicated.
- Do not alter the Common Day/Night palette, calendar resources, file type associations, navigation behaviour or package identity.

The standard GTK regression suite, Debian package validation and aggressive-native installer qualification must pass before publishing. Actual mouse/keyboard interaction in the user's desktop is a further confirmation, not substituted by headless tests.
