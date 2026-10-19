# Changelog

## 0.1.54 — 2026-10-19

- Bind asynchronous external clipboard Paste to its requested destination and refuse text completion after the window has been destroyed, preventing navigation or shutdown from retargeting a file mutation.
- Require beta APT's `SHA256` index entry to match the exact qualified Files Debian package, in addition to package identity, version and catalogue visibility.
- Qualify Debian maintainer-script execution, Common-provided extended-font helper and declared runtime prerequisites during CI package inspection.
- Reconcile the maintained Common integration ledger with the exact `1.19.66` gitlink and distinguish historical swarm roadmap entries from current release authority.


## 0.1.50 — 2026-10-18

- Fix unreadable Day-mode text on the Recent sidebar menu button: style its GTK4 inner button, label and icon with the active Common theme, including hover and open states.
- Qualify the actual Recent text in both Day and Night GTK windows and after independent per-window theme changes.
- Preserve sidebar geometry, all file operations, menu behaviour and shared MB Corpo typography.

## 0.1.49 — 2026-10-18

- Refine Files GTK4 typography after live Night screenshot review: stronger A Title Cond branding, clearer MB Corpo S Text navigation, and readable section labels and column headings.
- Keep S Text Cond for metadata while using regular S Text for sidebar place names, preserving distinction without illegible compression.
- Keep Common Day/Night semantics, file-operation behaviour, native row geometry and standalone 23-face provisioning unchanged.

## 0.1.48 — 2026-10-18

- Pin Libraries/Common 1.19.57 and install its verified 23-face native font bootstrap through Files' Debian configure hook.
- Make Files' MB Corpo Text, Condensed and Title typography available when installed standalone without depending on InfiltratorOS, while retaining the existing platform-native appearance policy and no package-owned font binaries.
- Preserve Files' copy/move/journal/recovery semantics and existing native installer optimisation profiles.

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
