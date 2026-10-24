# Changelog

## 0.1.62 — 2026-10-24

- Add a bounded, asynchronous inspection panel with live GIO metadata, image dimensions, extended-attribute listing, POSIX ACL presence and filesystem capacity details when available.
- Add cancellable SHA-256 computation and two-regular-file hash comparison, and allow visible evidence to be copied to the clipboard. Hash agreement is identified as digest agreement, not a byte-by-byte proof.
- Add native-folder terminal launching for the current and selected folder, using a working directory and an argument vector without shell interpolation.
- Add cancellable directory logical/allocated size and item counting, limited to 250,000 items and depth 64, refusing unsupported allocation values rather than inventing zero and never descending through symlinks or native mount boundaries.
- Integrate new interactions into an opt-in Inspection and tools overflow section and context-specific short menus, leaving the journalled mutation engines untouched; qualify core flows in headless GTK tests.
- Real Linux Mint screenshot acceptance and physical/provider stress testing remain outstanding.


## 0.1.61 — 2026-10-24

- Replace the unbounded right-click action catalogue with a bounded, compact, selection-aware GTK context menu, retaining the complete command surface in File actions.
- Distinguish empty-directory creation from file selection; show file/folder actions only for applicable types, suppress Restore outside Trash, and remove empty sections and orphaned dividers.
- Delegate Copy/Cut/Paste and all mutations to the existing qualified controllers; no separate file-operation implementation is introduced.
- Add headless UI regression coverage for popup bounds, contextual creation/restore visibility and clipboard-action delegation. Real Linux Mint visual inspection remains required for final presentation qualification.

## 0.1.60 — 2026-10-23

- Restore deterministic GitHub-hosted Ubuntu 24.04 release runners after organisation runner `BigBedroom-Linux-4` failed during `sudo apt-get update` with an interactive password request. Maintain the full dependency installation, installer execution, package integrity and publish checks rather than bypassing privileged operations.

- Select visible filenames by `*` / `?` pattern or by matching directory/content type.
- Open a multi-selection of folders in separate tabs after whole-request capacity verification.
- Copy multiple selected native paths or URIs as newline-delimited text, failing without changing the clipboard for remote-only native paths, unavailable metadata, selections over 4,096, or payloads over 1 MiB.
- Exercise the five new actions against real GTK selection and clipboard models. Ordinary file-reference clipboard Copy/Cut and journalled mutation behaviour remain unchanged.

## 0.1.59 — 2026-10-23

- Require an explicit Shift modifier and a same-process GTK drag for native Move, so offered drag-action flags cannot accidentally authorise source deletion. Prefer Copy on drag motion otherwise; route both through the qualified transfer engine.

## 0.1.58 — 2026-10-23

- Add explicit Shift+drag native moves through the journalled batch-transfer engine; only an in-process source and negotiated move can delete originals, while ordinary/external drops safely copy.
- Decode bounded remote GIO image thumbnails off the UI thread (8 MiB maximum, four concurrent decodes), with normal icon fallback for unavailable providers.
- Open bookmarked folders in a new tab or independent window from the bookmark options menu.
- Refresh the current folder with F5 or Ctrl+R without a navigation-history step, preserving the selected filenames.
- Persist up to 128 explicit per-folder Detail/Visual/Compact presentation choices with atomic writes and fail-closed corrupt-state handling, while leaving tabs and global fallback functional.
- Add workspace tests for navigation, multi-window view preferences, bookmark actions, refresh and malformed preference refusal; the full intended dual-pane, search, archive and permission work remains pending.

## 0.1.57 — 2026-10-23

- Qualify the URI-aware desktop launcher (`Exec=/Programs/files %U`, `MimeType=inode/directory;`) in both Debian and native-installer package/installation checks; reconcile shipped workspace and batch-removal capabilities with the README.
- Share progress and safe-boundary cancellation across single/batch transfers, clipboard Paste and drag/drop; honour cancellation while measuring trees and replacing conflicts, and refresh Cut ownership after partial completion.
- Open command-line paths/URIs asynchronously, reveal file arguments in their parent folder, and resolve typed relative paths against the current location.
- Move tabs between windows or into a new window while retaining history, selection and view mode; reject stale tab/window drag identities.
- Restore the last normally closed window's bounded browsing session on ordinary startup, validating all saved state before replacing the current workspace; never restore transient operations.
- Add bookmark labels and drag ordering with legacy import, atomic private persistence and propagation to other open windows.
- Apply Trash and confirmed permanent deletion to multiple selected native siblings, journalling each item and allowing cancellation between items; refuse late confirmation after window closure.
- Add workspace integration and destructive batch regressions. Keep the broader feature catalogue explicitly unfinished.

## 0.1.56 — 2026-10-23

- Refuse queued clipboard conflict choices after their originating window is removed from the application.
- Require successful mutation-lifetime admission before every controller dispatches a worker; release admission after journal setup failure.
- Qualify live and late Skip, Keep Both and Replace continuations for both Copy and Cut using the actual clipboard/dialog controllers, and reject duplicate or revoked mutation leases.
- Align the maintained temporal integration ledger with Common's ordinary host Gregorian adapter and optional chronology resources.

## 0.1.55 — 2026-10-23

- Flush private copied data, metadata and directory entries before publication, and the destination parent before cross-volume source removal. Flush failures retain the original and report exact recovery paths; cancellation remains effective before publication.
- Preserve bounded native extended attributes and POSIX ACLs for copied regular files/directories, remove destination-only inherited ACLs, and fail explicitly when required metadata cannot be preserved.
- Share directory-monitor suspension and window/process lifetime across creation, rename, single/batch transfer, destructive and clipboard controllers. Closing the last window keeps active mutations alive until their durable results are consumed.
- Use one journal-frame validator for recovery and mutation admission, reject malformed complete journals before append, and invalidate the bounded validation checkpoint after external edits.
- Record empty-file payload ownership before flush/close so failures cannot leave unreported private residue.
- Add transfer durability/metadata fault-injection and operation/window/application lifetime regressions; retain the explicit early-product scope and pinned Common revision.

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
