# Changelog

## 0.1.47 — 2026-10-18

- Optimise Linux Files typography around the 23-face native MB Corpo font cache: Text regular for filenames and reading, Text Cond for dense file metadata and sidebar entries, A Text Cond for breadcrumbs, S Title Cond Bold for section captions and S Text Cond Bold for column headings.
- Keep the stronger existing A Title Cond brand identity, Common palette, compact geometry and per-window Day/Night isolation.
- Retain pinned Common Title faces as fallback for independently installed Files before its optional extended resources are present; do not modify filesystem operations or installer profiles.

## 0.1.46 — 2026-10-18

- Align Files' product-title typography with Libraries/Common's canonical MB Corpo brand face and declared regular weight, removing an app-local synthetic 600-weight assumption and the explicit generic fallback on the title.
- Keep native GTK4 per-window System/Day/Night palette mapping and file-operation semantics unchanged.
- Standardize family documentation and root navigation; historical release-specific technical notes remain under `docs/RELEASE-*.md`.

## Earlier work

For the detailed engineering records of prior alpha versions, consult the dated `docs/RELEASE-*.md` files and immutable GitHub release history. This changelog does not redefine those releases.
