# Changelog

## 0.1.72 — 2026-10-25

- Correct the physical Linux Mint OS-reset failure where Files has a valid
  `0.1.71+native1` dpkg registration but the package-owned files under
  `/Programs` and `/System` were removed during an InfiltratorOS reinstall.
  APT's ordinary same-version `install` was a no-op; later file verification
  correctly failed even though the locally compiled native package was valid.
- Always pass `--reinstall` when installing the exact locally compiled native
  Debian package. This restores missing package-owned files on both first
  installation/upgrade and repeat installations without purging packages or
  changing the machine's default file-manager associations.
- Add CI regression reproducing the stale-dpkg/newly-empty native directory
  state, restoring files with a same-version local DEB, and a native installer
  contract check for forced same-version reinstall.
- Keep two-pass CPU-native PGO, the original `/Programs/files` binary,
  `/System/Resources/Documentation/Files/BUILD-INFO`, and Nemo untouched.


## 0.1.71 — 2026-10-25

- Fix the Mint InfiltratorOS native installation failure when GTK 4 build dependencies are already present but the `libarchive-dev` development headers and linker library are missing. Probe LibArchive explicitly before deciding to skip the APT build-dependency installation.
- Keep the independently installed Files application at `/Programs/files`, preserve Nemo and the system's default file-manager associations, and retain the two-pass CPU-native PGO installation flow.

## 0.1.70 — 2026-10-25

- Install Files-owned font bootstrap in `/System/Commands/files-ensure-fonts` and package build provenance in `/System/Resources/Documentation/Files`.
- Update Debian post-installation, aggressive native installer verification, and CI package assertions for the canonical InfiltratorOS namespace.
- Preserve `/Programs/files`, the desktop launcher and all existing Nemo/default file-manager and MIME associations; do not remove Mint's existing file manager.
- Introduce a new immutable package version rather than rewriting the already-published 0.1.69 release.

## 0.1.69 — 2026-10-25

- Add copy-only native ZIP creation for one selected regular file, using bounded private staging, journal admission, collision refusal and the existing durable transfer publication path. Directory-tree creation, encryption and virtual archive browsing remain outstanding.
- Introduce independent secondary-pane name sorting; preserve the displayed source identity in selection, transfers and drag/drop. Complete pane/tab/view parity is not yet qualified.
- Add synchronised Owner/Group/Others permission checkboxes beside the octal mode field, without silently clearing special bits. ACL editor, ownership changes and privilege elevation remain outside this release.
- Cap GIO provider imports at 1 GiB per file by metadata preflight and live transfer progress; identify remote disconnection and portable-device disappearance. Provider originals are not moved or deleted. Real MTP/PTP hardware qualification and recursive imports remain open.
- Extend native archive, provider import and GTK workspace tests. This bounded release is not completion of the 520-item original catalogue.

## 0.1.68 — 2026-10-24

- Correct the failure exposed by a real InfiltratorOS native installation after Files compiled successfully: APT rejected installing a locally compiled package under the generic release's Debian version as a downgrade.
- Give the final trained two-pass aggressive build a distinct `+native1` package version, leaving the published generic APT version and underlying project source VERSION unchanged for that release.
- Reject a genuinely newer installed version before rebuilding; do not mask version-ordering errors with unconditional `--allow-downgrades`.
- Make hosted native-installer CI first install the generic package, then require the local installer to upgrade it and verify the installed `+native1` version, profile, desktop entry and binary.



## 0.1.67 — 2026-10-24

- Publish the previously unreleased 24 post-v0.1.66 commits using a distinct immutable version. Do not retag or overwrite the earlier release.
- Add regular-file sidebar bookmarks and native missing/unavailable reference states; selection by visible filename pattern, file type, size and modification date; confirmed broken-link indicators.
- Add opt-in single-click activation, mouse history controls, configurable folder activation policy, bounded icon sizes and remote image thumbnail preference.
- Add validated atomic TSV reconstruction of the derived tag catalogue, apply private-location exclusions to Recent/activity, and persist bounded crash-safe browsing session checkpoints (without resuming file operations).
- Reaudit the complete 520-entry intended scope by catalogue section and reconcile original v0.1.61 requirements with current source, test evidence and remaining gaps in `docs/FEATURE-AUDIT-0.1.67.md`. This is **not** a claim of full feature completeness.
- Exact-source CI, publishing, native package identities and central APT digest are separate required gates; never infer publication solely from a passing Verify job.


## 0.1.66 — 2026-10-24

- Qualify four additional **original v0.1.61 catalogue requirements**: created-time sorting (line 165), hidden/special visual distinctions (181), optional filename-extension display (182), and symlink/shortcut identity markers (183).
- Add a Common-formatted Date Created column with correct raw-GIO sorting and honest unknown values; show presentation-only visual identity cues across list, icon and compact views.
- Add persisted Show/Hide filename extensions in View and layout, refresh realised labels in place and preserve full raw names for selection, rename, navigation and all mutations.
- Harden right-click pointer selection to resolve only authoritative `GFileInfo::name`, never a shortened or decorated label; unit and GTK regressions enforce this distinction.
- Together with the 11 bounded completions in 0.1.65, this advances **15 distinct original requirements**. The four partially delivered 0.1.65 requirements are not silently counted as complete; see `docs/FEATURE-AUDIT-0.1.66.md`.
- The exact code passed 20 hosted CTest executables and package/native-installer qualification; live desktop visual qualification remains outstanding.


## 0.1.65 — 2026-10-24

- Advance **15 distinct original Files feature-catalogue entries**, with traceability to the untouched v0.1.61 specification: 11 bounded complete and 4 honestly partial; see `docs/FEATURE-AUDIT-0.1.65.md`.
- Add selected-location GIO retrieval, owner UID and modified-range filters, bounded SHA-256 duplicate grouping, mandatory private-location exclusions, up to 16 saved searches and genuine real-object reveal.
- Add a derived, atomic URI/tag catalogue with 1–128 item bulk tagging, exact tag search, TSV export and strict refusal to overwrite unreadable state. Tags do not become filesystem authority.
- Add copyable SHA-256 verification (exact hash or basename-bound sha256sum line), byte-stream comparison with size/digest reporting and richer link/provider/volume inspection.
- Preserve bounded resource policies, cancellation, provider capability distinctions, explicit unknown/incomplete states and read-only search/inspection boundaries. No new mutations or elevated services.
- Full smart collections, comprehensive metadata collection filters, large/cross-root duplicates and imported tag-catalogue reconstruction remain unfinished. Native visual qualification remains open.


## 0.1.64 — 2026-10-24

- Add opt-in Quick Look (Space and context action) for regular files, preserving the existing open/launch and mutation ownership boundaries.
- Preview supported images, UTF-8 text up to 256 KiB, and bounded binary hex/ASCII evidence, exposing file name, type, size and availability.
- Navigate nearby visible files, zoom images, copy text or path and cancel reads. Refuse unbounded or unknown-size files rather than blocking the interface.
- Run GIO reads and image decoding away from GTK, hold source GFile references, fence stale asynchronous results by generation and reject late callbacks after dialog destruction.
- Qualify text, PNG image/zoom, binary fallback, copy and navigation against real GTK dialogs under the full hosted CTest and native-installer gates. Real Mint visual qualification remains separate.


## 0.1.63 — 2026-10-24

- Add a bounded, cancellable recursive search UI backed by the GIO provider layer, separate from the existing Ctrl+F current-folder quick filter.
- Provide exact/substring/glob name matching, Unicode case-folded versus case-sensitive search, file/folder filters, minimum/maximum size, modified-since local date and bounded text-content search.
- Reveal search results in their containing folder with original-item selection through the existing window navigation path, without duplicating file mutation machinery.
- Limit traversal to 100,000 entries, 500 hits and 64 levels, refuse symlink traversal and known cross-device descent, and disclose incomplete enumeration, provider failures and skipped text files.
- Qualify the core query filters and cancellation plus GTK search-dialog/reveal behaviour through the full 18-test hosted suite. Native installer and Debian qualification remain release gates; live desktop visual review remains outstanding.


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
