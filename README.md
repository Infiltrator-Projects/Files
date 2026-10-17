# Files

Files is a clean-sheet file-management project for InfiltratorOS.

**Current source version:** authoritative in [`VERSION`](VERSION)\
**Shared foundation:** exact Libraries gitlink, linked through the full Common target on InfiltratorOS/POSIX\
**Desktop implementation:** C++20 + GTK 4/GIO\
**Status:** early implementation; deterministic browsing and mutation foundation with multi-selection batch transfer, interruption inspection and verified metadata preservation

The project does not exist to reproduce Nemo, Dolphin, Explorer, Finder or another existing file manager. Mature products, standards and current research are evidence. The project chooses the strongest justified mechanisms and owns its own interaction, operation and recovery semantics.

The implementation has now started, but the architecture remains deliberately capable of changing while the early model and interaction contracts are being proven. The first tranches establish a real browser and the mutation boundary before semantic indexing, broad parity features or filesystem-specific policy are allowed to accumulate.

## Design position

Files is intended to complement InfiltratorFS while remaining a general-purpose file manager on other filesystems.

The key boundary is:

```text
filesystem exposes capability
        ↓
File Manager discovers and presents capability
        ↓
ordinary fallback remains available elsewhere
```

InfiltratorFS remains authoritative for its persistent format, transactions, allocation, integrity, history and storage policy. File Manager must not parse private InfiltratorFS structures or make ordinary operation depend on InfiltratorFS.

Where stable qualified interfaces exist, InfiltratorFS can enable stronger experiences such as persistent object identity, retained file history, reflink-aware copies, integrity information and richer recovery.

## Product principles

- first-principles engineering rather than competitor imitation;
- navigation, exact search and semantic/contextual retrieval as peer workflows;
- real files and the real filesystem namespace remain authoritative;
- semantic indexes are local, derived and rebuildable;
- deterministic file mutation with explicit failure and verification semantics;
- recovery/reversibility designed with the success path, not after it;
- capability discovery instead of filesystem-name assumptions;
- asynchronous first-paint-oriented UI behaviour;
- progressive disclosure: simple surface, deep capability;
- platform/toolkit details kept below a toolkit-neutral file/object model;
- Common is the canonical first-party substrate for strong product-neutral mechanics rather than a token dependency; and
- Mercedes-Benz-level technical excellence, reliability, effortless interaction and disciplined visual reduction as the execution quality bar.

## Complete intended product surface

The full feature catalogue is maintained in [`docs/FEATURES.md`](docs/FEATURES.md). It covers both mature file-manager parity and the capabilities that make Files an Infiltrator product: deterministic recoverable operations, exact/content/semantic retrieval, contextual activity, first-class InfiltratorFS identity/history/reflinks/integrity, remote/device/archive handling, security, accessibility, performance and qualification.

The catalogue is intentionally broader than the current release. It is the product contract used to plan implementation and to prevent local UI changes from silently becoming architecture.

Common's role is documented separately in [`docs/COMMON-INTEGRATION.md`](docs/COMMON-INTEGRATION.md). That ledger identifies which mechanics Files should inherit from Common, which semantics remain Files-owned, and which capabilities remain InfiltratorFS-owned.

Product-level acceptance is defined in [`docs/PRODUCT-QUALITY.md`](docs/PRODUCT-QUALITY.md). It establishes the golden user journeys, engineering/interaction/aesthetic evidence, proportional-complexity rule and release-quality gate used to decide whether a tranche actually makes Files better for a person.

## Current implementation

File modification timestamps use one shared temporal policy snapshot for both date and time. Explicit clocks use Common's UI-language-aware renderer: English Chinese-time labels use `Shēn hour` or `Shēn, first half`, Chinese-language labels use native characters, and Roman clocks keep Roman numerals and daylight twelfths with translated wording. Edo clocks use Japanese bell numerals and localized branch wording; explicit 12-hour markers and ordering follow the UI language. Selected non-Gregorian dates use Common's optional bridge to Calendar 1.0.86 or newer for matching UI-language presentation, including native Chinese/Dangi cyclic year names without an added Gregorian year or bare cycle counter; Calendar owns conversion. An unavailable date provider is shown as unavailable rather than silently displaying a Gregorian date. Locale Gregorian dates retain their native platform formatting. Existing live policy monitoring refreshes the visible labels; sorting continues to use raw filesystem timestamps.

The executable is `files`, presented to the user as **Files**.

It currently provides:

- Home, Desktop, Documents, Downloads, Computer and Trash entry points;
- live mounted local/removable places discovered through GIO, with shadowed implementation mounts excluded and each place opening at GIO's user-facing default location;
- mounted-place availability tracking that distinguishes the most-specific supplying mount for nested mounts, reports a neutral unavailable state when that source disappears and only auto-restores the current view when GIO provides matching root-and-UUID evidence;
- Back, Forward and Up navigation;
- an editable location field accepting paths and URIs;
- asynchronous, monitored directory enumeration through GTK/GIO;
- a virtualised GTK 4 list presentation rather than one widget per directory entry;
- shared-model **Detail/List**, **Visual/Icons** and **Compact** browse presentations with common selection and activation semantics;
- an analytical Detail/List presentation with aligned **Name**, **Type**, **Size** and **Date Modified** fields sourced from the same directory metadata model;
- a true Compact dense-scan composition using a small icon beside each filename so multiple readable columns can flow across the window;
- native file/folder icons, Common-formatted file sizes and item count/status;
- directory activation and launching files through the platform's registered default application;
- a toolkit-neutral `Location` model and navigation history beneath the GTK presentation;
- the canonical Infiltrator Day/Night/System design contract from pinned Common;
- a persistent **Follow system / Day / Night** appearance control, with live GTK system-theme tracking when Follow system is selected;
- a real GTK multi-selection model for selecting more than one directory item while preserving the existing single-item action contract when exactly one item is selected;
- deterministic **New Folder**, available from the header action or `Ctrl+Shift+N` for native locations;
- deterministic **Rename**, available from the selected-item Actions menu or `F2` when exactly one item is selected;
- deterministic single-item **Copy to…** and **Move to…**, using a native folder picker for the destination;
- isolated multi-item **Copy selected to…** and **Move selected to…** actions rather than expanding the proven single-item controller into a second state machine;
- full-batch collision preflight before mutation when no batch conflict policy has yet been chosen, so a detected destination conflict does not allow earlier non-conflicting items to be silently transferred first;
- compatible batch-conflict choices for **Skip Conflicts**, **Keep Both** and **Replace Conflicts**, with one selected policy applied consistently to conflicts in that batch;
- per-item durable START/END journal records inside batch operations, so interrupted work remains attributable to individual source/destination mutations;
- aggregate byte/item progress and safe-boundary cancellation for multi-item Copy/Move batches;
- explicit single-item destination-conflict choices for **Skip**, **Keep Both** and **Replace** rather than implicit overwrite behaviour;
- deterministic Keep Both naming such as `name (copy).ext`, `name (copy 2).ext` and subsequent unique names while preserving file extensions;
- destination names are claimed at the mutation boundary with no-replace semantics, so a competing writer is reported as a conflict rather than overwritten after preflight;
- browsing remains available on providers that cannot supply atomic no-replace; safe Rename, Move, private publication and replacement instead return a typed unsupported-capability result before changing the source or visible destination, while provider-specific atomic equivalents remain future work;
- failed or cancelled transfers remove only filesystem objects whose recorded identity still belongs to that operation, leaving competing-writer data intact;
- regular files, links and directory trees are assembled in descriptor-rooted private staging and only published after copy and metadata work succeeds; cancellation and setup failures leave no visible partial destination;
- cleanup, rollback and quarantine failures return structured exact retained-path evidence and include those paths in the calm user-facing result;
- replacement staging records the previous destination identity and first quarantines that exact object; an object swapped into the visible name is restored with no-replace semantics rather than stranded under an internal name;
- explicit **Replace** handling that stages the existing destination rather than deleting it first;
- replacement staging that verifies the replacement, restores the previous destination on a safe failure path and retains staged data rather than guessing after an uncertain move;
- same-filesystem moves through filesystem rename semantics, with copy-then-remove fallback for cross-volume moves;
- recursive directory transfer is assembled under descriptor-rooted protected staging and atomically published once complete, with symbolic links preserved as links;
- ordinary copy and cross-volume move transfer paths that preserve regular-file and directory permissions and modification timestamps when the destination permits them;
- metadata preservation applied to directories after their children are written so directory modification times are not immediately destroyed by the copy itself;
- explicit verification failure when copied contents exist but supported permission/timestamp metadata could not be preserved, rather than claiming perfect preservation;
- byte and item progress for single-item Copy/Move transfers, surfaced in the header without blocking the GTK main loop;
- explicit transfer cancellation, checked during chunked file copy and directory traversal, with partial copy destinations removed where cleanup is safe;
- cross-volume moves verify the complete copied output set, atomically quarantine the unchanged source, compare the stable source shape with the published copy and only then remove it; remaining source data is restored after a deletion failure when the original name is still free;
- cancellation remains effective during copy and traversal; a fully copied, metadata-preserved and identity-verified destination is the cross-volume move commit point and is reported as completed rather than as a late cancellation with duplicate data;
- deterministic **Move to Trash** for one selected native item, available from the Trash action and the `Delete` key;
- **Restore** from the Trash namespace to the original local path when the platform exposes the original location;
- explicit restore-collision handling with staged replacement rather than deleting the existing destination first;
- explicit **Delete Permanently…**, guarded by a confirmation dialog and available separately from recoverable Trash removal; `Shift+Delete` invokes the permanent path directly;
- a toolkit-neutral ordinary operation engine with explicit preflight, destination planning, execution, post-verification and typed result states;
- a dedicated transfer operation layer for conflict policy, progress accounting, cancellation and qualified metadata preservation rather than enlarging the ordinary operation engine;
- a separate batch-transfer engine that owns multi-source planning and policy while reusing the qualified single-item transfer/recovery layers;
- a separate recovery/destructive operation layer for replacement rollback, Trash, restoration and permanent deletion so destructive policy does not inflate the ordinary operation engine;
- an fsync-backed durable operation journal at the user's XDG state location, with a persisted START record before mutation and END record after the operation result;
- fail-closed journalling for New Folder, Rename, Copy, Move, Keep Both, Replace, Trash, Restore, Permanent Delete and each item in a batch: if the START record cannot be persisted, that mutation is not started;
- startup inspection of journal START records with no matching END from a previous Files session;
- conservative interrupted-operation closure as **verification failure**, never inferred success, while retaining the exact operation kind/source/destination for inspection;
- an explicit warning/inspection surface when interrupted operations are found, including notification if the recovery END itself cannot be persisted;
- journal completion failures surfaced as verification problems instead of silently inventing a durable completed state;
- distinct handling for invalid requests, destination conflicts, permission failures, read-only locations, unsupported safe-operation capabilities, cancellation, execution failures and verification failures;
- asynchronous execution of mutation work so filesystem operations do not block the GTK main loop;
- monitored refresh and automatic selection of newly created, renamed, copied or moved single items when their result appears in the current view;
- separate creation, single-item operation, batch operation, destructive/recovery, journal-inspection and theme controllers so interaction policy does not accumulate inside `FileManagerWindow`;
- the full `InfiltratrCommon::Common` build dependency on InfiltratorOS/POSIX, making Common's POSIX/state/I/O contracts available to the non-UI layers as they are introduced; and
- a hosted build/test gate on every main-branch update, including headless GTK window lifetime, sorted selection, mount-row identity/focus and fresh-profile policy monitoring, mounted-place identity qualification, batch preflight/policy/progress/cancellation, interrupted-journal recovery and permission/timestamp preservation qualification.

Metadata preservation is deliberately honest in the current implementation: permissions and modification timestamps are qualified for ordinary copy/cross-volume transfer paths, while ownership, ACLs, extended attributes, sparse-file preservation and symlink metadata remain future capability work. Multi-selection batch Copy/Move is present; destructive batch Trash/Delete is not yet claimed.

The next implementation work continues the richer file/object/capability model and remote-location/provider handling; mounted/removable discovery is already part of the browsing foundation rather than a reason to add ad-hoc controls.

Semantic/context indexing and InfiltratorFS-specific capability providers are also deliberately absent from this early executable. Ordinary file-manager correctness comes first.

## Build

On a Debian-family development system with GTK 4 development files installed. Install `xvfb`, `xauth` and `dbus-x11` to run the headless GTK regression test as CI does:

```bash
git clone --recurse-submodules https://github.com/Infiltrator-Projects/Files.git
cd Files
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/files
```

The build rejects a missing or incompatible Common dependency and verifies its exact Git commit against the recorded gitlink. CMake declares the minimum API version; the gitlink owns the selected revision.

## Design documents

- [Validation](docs/VALIDATION.md) — current test, packaging and release contracts.
- [Philosophy](docs/PHILOSOPHY.md) — canonical engineering standard.


- [`docs/FEATURES.md`](docs/FEATURES.md) — complete intended feature catalogue and release sequencing.
- [`docs/PRODUCT-QUALITY.md`](docs/PRODUCT-QUALITY.md) — golden user journeys and the acceptance discipline for proving that a tranche improves the product.
- [`docs/COMMON-INTEGRATION.md`](docs/COMMON-INTEGRATION.md) — Common ownership and integration ledger.
- [`docs/DESIGN.md`](docs/DESIGN.md) — product philosophy, InfiltratorFS relationship, quality bar and non-goals.
- [`docs/RESEARCH-2026.md`](docs/RESEARCH-2026.md) — research review covering file retrieval, semantic systems, context, workflow, recoverability and interface design.
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — capability-based architecture, deterministic operation boundary and qualification model.

## Research conclusion

The evidence reviewed so far does **not** support replacing folders with an AI interface.

It supports a stronger hybrid:

```text
navigation ─┐
exact search ├─→ authoritative file/object model → deterministic operations
semantic    ─┘                         ↓
                                  OS/filesystem
```

Context, semantic retrieval, activity views and related-item discovery can improve re-finding, but they remain derived overlays. The filesystem stays the source of truth.

## Implementation sequence

The implementation proceeds by proving boundaries rather than accumulating visible features:

1. read-only navigation shell and toolkit-neutral location model, with Common used for every applicable product-neutral contract;
2. deterministic mutation/operation engine with explicit failure and verification state;
3. richer file/object/capability model and volume/remote-location handling;
4. mature parity features such as tabs, split view, bookmarks, properties, permissions, thumbnails/previews, archives and batch rename;
5. exact/content search and rebuildable indexing;
6. qualified InfiltratorFS capability provider for persistent identity, native history, reflinks, integrity and stronger recovery;
7. contextual/semantic retrieval as a rebuildable non-authoritative layer; and
8. continued accessibility, performance, visual and interaction refinement against the Common/Infiltrator design contract.

Each tranche must remain useful and testable without making later architectural layers authoritative by accident.

Current browsing foundation discovers mounted/removable places through GIO, keeps them deterministically ordered, blocks nested-mount fall-through when a supplying mount disappears, and restores an unavailable current view automatically only when the returning mount's root and provider UUID prove the same source.
