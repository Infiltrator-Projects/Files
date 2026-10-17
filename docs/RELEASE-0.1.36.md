# Files 0.1.36 — Everyday interaction tranche

This version qualifies five ordinary file-manager usability features without
changing the proven deterministic mutation, journal or recovery engines.

1. Inline rename in Detail/List, Icons and Compact presentations. F2 and Rename
   enter the in-place editor and initially select the base filename without its
   extension. Validation and mutation still run through ItemOperationController
   and OperationEngine.
2. File selection drag/drop onto directories and pinned/system sidebar locations.
   This tranche deliberately negotiates COPY only. Compatible local file-list
   imports are supported; deleting an external drag source without a verified
   transfer contract would be unsafe. Move to… and Cut/Paste continue to use
   the safe existing move path.
3. Persistent user bookmarks / pinned places kept separately from live mounts,
   with pin/unpin, move up/down and remove controls.
4. Native image thumbnails decoded off the GTK main loop and applied only while
   a cell still represents the original file/version. The cache is bounded, and
   unreadable/large/remote formats fall back to file icons.
5. Multiple tabs, independent navigation history and remembered selection and
   view-mode state, with Ctrl+T/Ctrl+W and open-directory-in-new-tab.

The release also includes the previously merged right-click context actions,
consolidated header, breadcrumbs, Properties, and Ctrl+C/Ctrl+X/Ctrl+V clipboard
operations from unreleased main development after Files 0.1.35.

CI compiles the C++20/GTK4 application, runs core and headless UI tests, builds
and validates the Debian package, and qualifies the aggressive native installer.
The canonical executable remains /Programs/files. This release does not change
the desktop's default file manager.
