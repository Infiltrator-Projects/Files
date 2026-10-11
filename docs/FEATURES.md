# Infiltrator File Manager — Complete Feature Catalogue

**Catalogue date:** 6 October 2026  
**Scope:** intended product capability, not only the current 0.1.0 implementation

This document is the authoritative feature catalogue for Infiltrator File Manager (Files). It exists so implementation does not drift into a sequence of locally convenient UI changes. Every substantial feature should map back to one of these product capabilities or cause this catalogue to be deliberately amended.

The catalogue is intentionally broader than a conventional Linux file manager. Files is a clean-sheet InfiltratorOS application designed around the strongest justified behaviour from mature file managers, current HCI/retrieval research, the Infiltrator software family and InfiltratorFS.

This is not a promise that every item ships in the first release. It is the complete intended product surface against which releases can be planned and omissions can be explicit.

## Status vocabulary

- **FOUNDATION** — already present in the initial 0.1.x foundation or required by the architecture now.
- **CORE** — expected ordinary file-manager capability; absence would leave Files incomplete as a general-purpose manager.
- **ADVANCED** — high-value capability beyond minimum parity, intended after the core is qualified.
- **RESEARCH** — direction justified by research but requiring prototype/measurement before the interaction contract is frozen.
- **IFS** — capability that becomes richer on InfiltratorFS but must degrade cleanly elsewhere.
- **DELEGATED** — intentionally handed to another InfiltratorOS product or operating-system service rather than duplicated in Files.

---

# 1. Product shell and session model

## Windows and sessions

- **FOUNDATION** Launch a normal top-level Files window quickly without synchronous directory scanning.
- **CORE** Multiple independent Files windows.
- **CORE** Open location in new window.
- **CORE** Tabs.
- **CORE** Open location in new tab.
- **CORE** Close, reorder, duplicate and move tabs between windows.
- **CORE** Reopen recently closed tab.
- **CORE** Restore useful session state after normal application restart.
- **ADVANCED** Restore interrupted browsing state after application crash without restoring unsafe transient operations as completed.
- **CORE** Per-window navigation history.
- **CORE** Per-tab navigation history.
- **ADVANCED** Optional split view / dual pane with independent locations and selection.
- **ADVANCED** Move/copy between panes by direct interaction without changing source location.
- **CORE** New-window and new-tab behaviour configurable without multiplying permanent toolbar controls.

## Startup and first paint

- **FOUNDATION** Application chrome paints before slow devices, thumbnails, remote locations or indexes complete.
- **FOUNDATION** Directory enumeration is asynchronous.
- **CORE** Session restoration does not block first paint.
- **CORE** Dead network locations cannot freeze the application shell.
- **CORE** Slow/removable media cannot freeze navigation chrome.
- **CORE** Cancel superseded enumeration/metadata work when the user navigates elsewhere.
- **CORE** Progressive population of very large directories.

---

# 2. Navigation and location model

## Conventional navigation

- **FOUNDATION** Back.
- **FOUNDATION** Forward.
- **FOUNDATION** Up / parent.
- **FOUNDATION** Home.
- **FOUNDATION** Filesystem root / Computer.
- **FOUNDATION** Direct editable path/URI entry.
- **CORE** Breadcrumb/path-bar navigation.
- **CORE** One-action switch between breadcrumb and editable path.
- **CORE** Keyboard focus shortcut for location entry.
- **CORE** Open parent while retaining child selection where practical.
- **CORE** Preserve sensible scroll/selection state when returning through history.
- **CORE** Navigation to symlink targets without losing clarity about the link object itself.
- **CORE** Relative path handling where a user explicitly enters it.
- **CORE** `~` and ordinary home semantics on supported POSIX platforms.
- **CORE** Correct URI-based locations through the platform provider rather than assuming every location is a POSIX pathname.

## Sidebar / places

- **FOUNDATION** Home.
- **FOUNDATION** Desktop when defined.
- **FOUNDATION** Documents when defined.
- **FOUNDATION** Downloads when defined.
- **FOUNDATION** Computer/root.
- **FOUNDATION** Trash.
- **CORE** User bookmarks / pinned places.
- **CORE** Drag to reorder bookmarks.
- **CORE** Rename bookmark label without renaming the target directory.
- **CORE** Mounted local volumes.
- **CORE** Removable devices.
- **CORE** Remote/network locations.
- **CORE** Recently used locations.
- **ADVANCED** Project/work-context locations derived from activity without altering the filesystem hierarchy.
- **CORE** Hide sidebar categories that contain no meaningful entries.
- **CORE** Device state shown only when a real device/location exists; no phantom categories.

## Bookmarks, favourites and collections

- **CORE** Bookmark any browseable location.
- **CORE** Pin important files as well as folders without moving them.
- **ADVANCED** Named collections containing references to files from multiple locations.
- **ADVANCED** Collections survive rename/move when the underlying platform exposes stable identity.
- **IFS** InfiltratorFS object identity used to make collections resilient to pathname changes.
- **CORE** Broken references are identified honestly rather than silently retargeted.

---

# 3. Views and visualisation

## Primary directory views

- **FOUNDATION** Virtualised list view.
- **CORE** Detailed list/table view with configurable columns.
- **CORE** Icon/grid view.
- **CORE** Compact view for dense directories.
- **ADVANCED** Gallery/media-oriented view where content warrants it.
- **CORE** View choice remembered globally and optionally per location.
- **CORE** Zoom/density control appropriate to the current view.
- **CORE** Responsive layout that remains usable at narrow window widths.

## Sorting and grouping

- **CORE** Sort by name.
- **CORE** Sort by type.
- **CORE** Sort by size.
- **CORE** Sort by modified time.
- **CORE** Sort by created time when the filesystem exposes it.
- **CORE** Sort ascending/descending.
- **CORE** Directories-first option.
- **CORE** Natural numeric filename ordering.
- **CORE** Locale-aware human name ordering where appropriate, with deterministic non-locale ordering for protocol/identifier fields where required.
- **ADVANCED** Group by date/type/size or other useful metadata.
- **ADVANCED** Stable sorting while asynchronous metadata arrives, avoiding gratuitous item jumping.

## Visibility

- **CORE** Show/hide hidden files.
- **CORE** Clear distinction between hidden, system/special and ordinary entries where the platform supports those concepts.
- **CORE** Optional display of file extensions without falsifying the real filename.
- **CORE** Symlink/shortcut/link state visually identifiable.
- **CORE** Broken link state visually identifiable.
- **CORE** Mount points and special namespace locations identifiable without visual noise.

## Thumbnails and previews

- **CORE** Asynchronous thumbnails.
- **CORE** Thumbnail generation never blocks browsing.
- **CORE** Thumbnail cache with invalidation based on file identity/version metadata rather than filename alone where possible.
- **CORE** Configurable thumbnail size and remote-file policy.
- **CORE** Image thumbnails.
- **CORE** Video thumbnails when a qualified provider is available.
- **CORE** PDF/document first-page thumbnails when a qualified provider is available.
- **ADVANCED** Audio artwork/waveform or media metadata where useful.
- **ADVANCED** Fast preview / Quick Look style action without launching a full application.
- **ADVANCED** Preview pane that is opt-in and collapsible.
- **CORE** Preview failure must not affect file-manager stability.
- **CORE** Preview providers execute with bounded resource policy and no mutation authority.

---

# 4. Selection and direct manipulation

- **FOUNDATION** Single selection.
- **CORE** Multi-selection.
- **CORE** Range selection.
- **CORE** Keyboard selection.
- **CORE** Select all / select none / invert selection.
- **ADVANCED** Selection by pattern/type/date/size.
- **CORE** Rubber-band selection in icon/grid view.
- **CORE** Drag selection without accidental file activation.
- **CORE** Drag and drop inside a directory.
- **CORE** Drag and drop between locations/windows/tabs/panes.
- **CORE** Drag to sidebar destination.
- **CORE** Modifier semantics for copy/move/link are explicit and platform-consistent.
- **CORE** Drop target communicates the operation before commit.
- **CORE** No destructive operation is triggered merely by hover.

---

# 5. File and directory creation

- **CORE** Create folder.
- **CORE** Create empty file where appropriate.
- **CORE** Create from templates.
- **CORE** Immediate inline rename after creation.
- **CORE** Validate names against the actual destination filesystem/provider rather than a hard-coded legacy filename rule.
- **CORE** Preserve full supported Unicode namespace.
- **CORE** Explain destination-specific naming restrictions when a provider rejects a name.
- **ADVANCED** Create common links/shortcuts from the UI where meaningful.
- **ADVANCED** Create sparse file only as an explicit advanced action, never accidentally.

---

# 6. Rename

- **CORE** Inline rename.
- **CORE** Keyboard rename.
- **CORE** Preserve extension selection behaviour intelligently without hiding the real name.
- **CORE** Collision detection before commit where the provider can supply it.
- **CORE** Case-only rename handled correctly on case-insensitive/case-preserving filesystems.
- **CORE** Unicode and normalisation edge cases handled without silently changing user data.
- **CORE** Rename failure leaves the original object intact and clearly selected.
- **ADVANCED** Batch rename with preview.
- **ADVANCED** Batch rename templates: prefix/suffix, numbering, replace, case, metadata/date substitutions.
- **ADVANCED** Batch rename conflict preview before any mutation.
- **IFS** Persistent object identity allows history/bookmarks/collections to follow a renamed object.

---

# 7. Copy, move, duplicate and link

## Ordinary operations

- **CORE** Copy.
- **CORE** Move.
- **CORE** Duplicate in place.
- **CORE** Cut/copy/paste clipboard workflow.
- **CORE** Drag/drop copy/move.
- **CORE** Same-filesystem rename/move takes the strongest atomic path available.
- **CORE** Browsing remains available when a provider lacks atomic no-replace; affected mutations fail closed with a typed unsupported-capability result and leave source, visible destination and hidden staging paths unchanged. Provider-specific atomic equivalents remain future work.
- **CORE** Cross-filesystem move is implemented as verified copy plus source removal with explicit partial-failure state.
- **CORE** Regular files, symbolic links and directory trees remain private until fully copied and metadata-preserved, then publish atomically without replacing an existing destination.
- **CORE** Failure, cancellation, cleanup and rollback results carry structured exact paths for every retained owned or quarantined object; visible messages surface the same recovery evidence.
- **CORE** Preserve timestamps when policy and destination allow.
- **CORE** Preserve permissions/ownership when policy and authority allow.
- **CORE** Preserve ACLs and extended attributes where supported.
- **CORE** Preserve sparse files rather than expanding holes unnecessarily.
- **CORE** Preserve symbolic links as links by default during normal filesystem copying rather than accidentally dereferencing them.
- **ADVANCED** Preserve hard-link relationships during tree copy when feasible.
- **CORE** Copying an unsupported metadata class reports the degradation rather than silently claiming perfect preservation.

## Links and clones

- **CORE** Create symbolic link where supported.
- **ADVANCED** Create hard link where supported and semantically safe.
- **ADVANCED** Reflink/clone copy when exposed by the filesystem/provider.
- **IFS** InfiltratorFS reflink-aware duplicate/copy path.
- **CORE** Reflink is an optimisation/capability, not a different user-visible file type.
- **CORE** Safe fallback to ordinary copy when clone/reflink is unavailable.

## Conflict handling

- **CORE** Replace.
- **CORE** Skip.
- **CORE** Keep both / automatic non-destructive rename.
- **CORE** Apply choice to remaining compatible conflicts.
- **CORE** Compare source and destination metadata before choosing.
- **ADVANCED** Side-by-side preview for ambiguous conflicts.
- **CORE** Directory merge with clear distinction from directory replacement.
- **CORE** Never equate matching filename with matching object/content.

---

# 8. Delete, Trash and restoration

- **FOUNDATION** Browse Trash.
- **CORE** Move to Trash/recycle facility when supported.
- **CORE** Restore from Trash to original location when known.
- **CORE** Resolve restore collisions explicitly.
- **CORE** Empty Trash.
- **CORE** Permanent delete as a distinct explicit action.
- **CORE** Permanent deletion clearly distinguishes recoverable Trash from non-Trash removal.
- **CORE** Files does not advertise cryptographic/secure erase guarantees that storage hardware or CoW filesystems cannot actually provide.
- **CORE** Deletion progress and partial failures are explicit.
- **CORE** Removal of large trees is cancellable at safe boundaries.
- **ADVANCED** Undo recent Trash operations while references remain valid.
- **IFS** Historical retained versions may permit stronger recovery after content replacement or deletion when the filesystem exposes a qualified recovery interface.

---

# 9. Deterministic operation engine

This is a defining product feature, not an internal implementation detail.

Every mutating operation follows the conceptual sequence:

```text
preflight → plan → execute → durability → verify → recover/undo state
```

Required capabilities:

- **CORE** One operation model for UI, keyboard, drag/drop and future automation paths.
- **CORE** Preflight source/destination existence and capability checks.
- **CORE** Explicit operation plan before destructive mutation.
- **CORE** Stable operation identity.
- **CORE** Accurate byte/item progress where knowable.
- **CORE** Indeterminate progress where total work is genuinely unknowable.
- **CORE** Pause where the underlying operation can pause safely.
- **CORE** Resume where supported.
- **CORE** Cancellation with clearly defined safe points.
- **CORE** Retry failed sub-operations without restarting unaffected completed work when safe.
- **CORE** Operation queue visible without becoming permanent UI clutter.
- **CORE** Concurrent operations bounded by device/network characteristics rather than arbitrary unbounded parallelism.
- **CORE** Per-device/per-destination scheduling to avoid destructive seek thrash or network overload.
- **CORE** Partial-output naming/state prevents incomplete files from masquerading as successfully completed originals where the provider permits staging.
- **CORE** Durability stage distinct from bytes-written stage.
- **CORE** Post-operation verification appropriate to operation type.
- **CORE** Operation log/journal sufficient to explain incomplete work after interruption.
- **CORE** Crash recovery does not invent success.
- **CORE** Hot-unplug/network-disconnect states remain recoverable or explicitly failed.
- **ADVANCED** Resumable large local/remote copies when source/destination/provider identity proves continuation is safe.
- **ADVANCED** Optional content verification/checksum for high-assurance copies.
- **ADVANCED** Undo/redo framework for operations whose inverse is safe and still valid.
- **IFS** Exploit stronger InfiltratorFS transaction/history capabilities when exposed, without bypassing the operation engine.

---

# 10. Search and retrieval

## Exact search

- **CORE** Filename search.
- **CORE** Partial-name search.
- **CORE** Exact-name search.
- **CORE** Case-sensitive/insensitive options where meaningful.
- **CORE** Search current folder.
- **CORE** Search recursively beneath current folder.
- **CORE** Search selected locations.
- **CORE** Search all indexed user locations.
- **CORE** Metadata filters: type, size, dates, owner, tags/collections, location.
- **CORE** Content search for indexable text/document formats.
- **CORE** Search-as-you-type without turning every keystroke into unbounded filesystem work.
- **CORE** Search results remain real references to filesystem objects.
- **CORE** Reveal result in containing folder.
- **CORE** Search result explains path/location.
- **ADVANCED** Saved searches / smart collections.
- **ADVANCED** Duplicate/content-hash search.

## Semantic retrieval

- **RESEARCH** Natural-language query over locally indexed filenames, metadata and supported content.
- **RESEARCH** Semantic similarity search.
- **RESEARCH** Query such as “the transmission capture I was working on last month” using explainable context.
- **RESEARCH** Result explanation: why this object matched.
- **RESEARCH** Semantic search can be disabled completely.
- **RESEARCH** Semantic index is local-first by default.
- **RESEARCH** Semantic model/index is derived and rebuildable.
- **RESEARCH** Semantic data never becomes namespace authority.
- **RESEARCH** No LLM or embedding subsystem may silently rename, move, overwrite or delete a file.
- **ADVANCED** Optional external model/provider only through explicit privacy policy and user control; ordinary Files remains complete without it.

## Contextual retrieval

- **RESEARCH** Activity windows: files opened/modified together.
- **RESEARCH** Project/application associations.
- **RESEARCH** Related files based on content, chronology and explicit user relationships.
- **RESEARCH** “Continue working” surface based on transparent activity evidence rather than opaque recommendation.
- **IFS** Stable object identity keeps context attached across rename/move.
- **CORE** Derived context can always be cleared/rebuilt without harming files.

---

# 11. Recent, activity and workflow surfaces

- **CORE** Recent files.
- **CORE** Recent folders/locations.
- **CORE** Clear recent-history controls.
- **ADVANCED** Recently modified by time range.
- **ADVANCED** Recently downloaded/received where origin metadata exists.
- **RESEARCH** Activity clusters representing work sessions/projects without changing physical location.
- **RESEARCH** Resume a previous activity by reopening its relevant locations/files.
- **CORE** Private/sensitive locations can be excluded from activity/index history.

---

# 12. Metadata, information and properties

## Standard properties

- **CORE** Real filename and display name.
- **CORE** Path/URI/location.
- **CORE** Type/content type.
- **CORE** Logical size.
- **CORE** Allocated size when available.
- **CORE** Modified time.
- **CORE** Created/birth time when available.
- **CORE** Access time only when available/useful, without forcing reads that perturb it.
- **CORE** Owner/group.
- **CORE** POSIX permissions where relevant.
- **CORE** ACL summary and editor where platform support is qualified.
- **CORE** Extended attributes / named metadata where useful and safe to expose.
- **CORE** Symbolic-link target and link status.
- **CORE** Filesystem/volume/provider.
- **ADVANCED** Content hashes/checksums on demand.
- **ADVANCED** MIME/media/document metadata through bounded providers.
- **ADVANCED** Image dimensions/EXIF summary.
- **ADVANCED** Audio/video duration and codec summary.

## Directory information

- **CORE** Item count when enumeration is complete.
- **CORE** Recursive size is an explicit asynchronous calculation, never implied to be instant.
- **CORE** Recursive size calculation cancellable.
- **ADVANCED** Logical versus allocated tree size.
- **ADVANCED** Shared/reflink-aware physical-cost information where the filesystem exposes it accurately.

## InfiltratorFS properties

- **IFS** Persistent 128-bit object identity when exposed through a stable user-space interface.
- **IFS** Historical generation/version count.
- **IFS** Integrity/checksum state where a qualified read-only interface exists.
- **IFS** Shared/reflink state where meaningful and accurately reportable.
- **IFS** Storage/protection intent where supported.
- **IFS** Never expose private on-disk implementation structures as if they were stable application API.

---

# 13. History, versions and recovery

- **ADVANCED** File history surface when the backing filesystem/provider exposes previous versions.
- **ADVANCED** Preview historical version read-only.
- **ADVANCED** Compare metadata between versions.
- **ADVANCED** Restore selected historical version through the deterministic operation engine.
- **IFS** Native InfiltratorFS retained generations become the preferred history provider when qualified.
- **CORE** History UI is absent or honestly unavailable when no provider exists; Files does not fabricate `.bak` semantics.
- **ADVANCED** Snapshot/version navigation can reveal the historical location/name if that information is available.
- **CORE** Restore never overwrites the current object without an explicit collision/replace plan.

---

# 14. Devices, volumes and removable media

- **CORE** Discover mounted volumes.
- **CORE** Discover mountable removable devices.
- **CORE** Mount.
- **CORE** Unmount.
- **CORE** Eject/power-off where supported.
- **CORE** Clear busy-device explanation when unmount/eject fails.
- **CORE** Capacity/free-space display.
- **CORE** Filesystem type/name when reliably available.
- **CORE** Volume label.
- **CORE** Read-only state.
- **CORE** Hotplug update without restart.
- **CORE** Device removal during active operation produces explicit recoverable failure state.
- **ADVANCED** Safely disconnect workflow waits for Files-owned operations and relevant flush completion before signalling success.
- **DELEGATED** Formatting, partitioning and filesystem creation belong to the disk/filesystem-management product, not Files.
- **DELEGATED** Defragmentation belongs to Infiltrator Defragmenter.
- **DELEGATED** Deep filesystem repair/scrub administration belongs to the filesystem/maintenance product even if Files links to it contextually.

---

# 15. Remote and network locations

- **CORE** Browse remote locations through the platform/provider layer.
- **CORE** SMB/CIFS where supported by the platform stack.
- **CORE** SFTP/SSH file access where supported.
- **CORE** WebDAV where supported.
- **ADVANCED** NFS browsing/mount integration where appropriate.
- **ADVANCED** FTP only where a platform provider supports it, with clear security limitations; do not make insecure FTP a first-class recommended path.
- **CORE** Saved network locations/bookmarks.
- **CORE** Credential requests use the operating system's qualified secret/credential service rather than Files storing plaintext passwords.
- **CORE** Connection failure and authentication failure distinguished.
- **CORE** Remote latency cannot block UI.
- **CORE** Operations expose network disconnect/retry state.
- **ADVANCED** Resume remote transfers only when provider/source/destination semantics prove continuation safe.
- **ADVANCED** Offline/cached metadata only where a provider can make freshness explicit.

---

# 16. Phones, cameras and portable devices

- **CORE** MTP device browsing when supported.
- **ADVANCED** PTP/camera import integration where supported.
- **CORE** Device appears only when present.
- **CORE** Copy/import uses the same operation model and reports device disconnect distinctly.
- **ADVANCED** Import rules can organise copies without modifying originals unless explicitly requested.
- **DELEGATED** Device synchronisation suites remain separate products/services.

---

# 17. Archives and packaged content

- **CORE** Extract common archives through a qualified provider/tool.
- **CORE** Create common archives.
- **CORE** Open/browse archive without forcing extraction when the provider supports safe virtual browsing.
- **CORE** Drag/copy files into/out of archive where supported.
- **CORE** Archive password prompts do not persist secrets accidentally.
- **CORE** Path traversal and unsafe archive entries are blocked during extraction.
- **ADVANCED** Preview archive contents and aggregate size before extraction.
- **ADVANCED** Verify archive checksums/signatures where the format/provider supports them.
- **CORE** Archive engine failure cannot corrupt ordinary file-manager state.

---

# 18. Open, launch and application association

- **FOUNDATION** Open file with default application.
- **CORE** Open With chooser.
- **CORE** Set/change default application through the operating-system association mechanism.
- **CORE** Open executable/script with clear execute-versus-view semantics.
- **CORE** Do not infer trust solely from filename extension.
- **CORE** Open containing folder from external URI/request.
- **CORE** Command-line invocation with files/locations.
- **ADVANCED** Open selected folder in terminal.
- **ADVANCED** Open terminal at current location.
- **ADVANCED** Share/send integration through OS portal/service when available.
- **CORE** External application launch failure is surfaced without destabilising Files.

---

# 19. Permissions, ownership and privileged operations

- **CORE** Read permission summary.
- **CORE** Edit ordinary permissions when authorised.
- **ADVANCED** ACL editor where platform semantics warrant it.
- **CORE** Ownership changes require explicit authority and platform policy.
- **CORE** Privileged operations use a narrow privilege broker/elevation path; Files itself should not run permanently as root/administrator.
- **CORE** No “open whole file manager as root” architecture as the normal administrative model.
- **CORE** Permission errors identify the object/action that failed.
- **CORE** Privilege elevation is scoped to the requested operation and auditable.

---

# 20. Clipboard and interoperability

- **CORE** Copy file references to clipboard.
- **CORE** Cut/move clipboard semantics.
- **CORE** Paste into compatible destination.
- **CORE** Copy pathname/URI as text.
- **CORE** Paste file lists from compatible applications.
- **CORE** Clipboard data does not become an authority source after referenced objects change; operation preflight revalidates them.
- **ADVANCED** Copy metadata/details as text where useful.

---

# 21. File comparison, checksums and integrity

- **ADVANCED** Calculate common cryptographic hashes on demand.
- **ADVANCED** Compare selected files by size/hash/content through a bounded worker.
- **ADVANCED** Verify downloaded/provided checksum text.
- **IFS** Surface filesystem-provided integrity state when qualified.
- **CORE** Distinguish an application-calculated content hash from filesystem integrity metadata.
- **DELEGATED** Full filesystem scrub/repair remains outside Files.

---

# 22. Tags, labels and user metadata

- **ADVANCED** User tags/labels without changing pathname.
- **ADVANCED** Search/filter by tag.
- **ADVANCED** Tag multiple selected objects.
- **ADVANCED** Use native filesystem metadata when a portable, stable contract exists; otherwise keep derived catalogue data clearly separate from filesystem truth.
- **IFS** Persistent identity lets Files associate derived labels robustly across rename/move.
- **CORE** Derived tag database is rebuildable/exportable and never required to open ordinary files.

---

# 23. Custom actions, extensions and providers

- **ADVANCED** Bounded extension/provider interface rather than arbitrary UI injection.
- **ADVANCED** Preview providers.
- **ADVANCED** Metadata providers.
- **ADVANCED** Search/index providers.
- **ADVANCED** Share/action providers.
- **ADVANCED** Filesystem capability providers.
- **ADVANCED** User-defined context actions with explicit command preview/arguments.
- **ADVANCED** Dynamic provider loading uses Common's qualified dynamic-library binding contract where applicable.
- **CORE** Provider crash/failure does not become authoritative application state.
- **CORE** Extensions cannot silently redefine core operation semantics.
- **CORE** Extension API versioning and capability declaration.
- **CORE** Disable problematic providers without losing ordinary browsing.

---

# 24. InfiltratorFS integration

Files and InfiltratorFS are designed to complement one another, but Files must remain fully useful on other filesystems.

- **IFS** Capability discovery rather than filesystem-name branching.
- **IFS** Persistent object ID used for stable references when exposed.
- **IFS** Follow object through rename/relocation in recents, collections and contextual indexes.
- **IFS** Native retained-history/version browser.
- **IFS** Restore historical object version.
- **IFS** Reflink/clone-aware duplication.
- **IFS** Sparse-file preservation and honest allocated/logical-size presentation.
- **IFS** Integrity state presentation.
- **IFS** Optional storage-intent hints such as normal/temporary/performance/archive if InfiltratorFS exposes a stable application-intent contract.
- **IFS** Protection/redundancy class display/request only when a stable user-space contract exists.
- **IFS** Stronger operation recovery when filesystem transactions/history can support it.
- **IFS** Efficient identity-aware semantic/activity indexing.
- **IFS** No private parsing of checkpoints, allocation trees, extents or other on-disk internals.
- **IFS** Files never decides allocation/compression/placement policy; it communicates user/application intent and the filesystem owns policy.
- **DELEGATED** Scrub, repair, defrag, low-level allocation inspection and forensic administration stay with specialist tools.

---

# 25. Infiltrator Common integration

Common is a first-class part of the Files architecture, not merely a utility submodule. The detailed ownership ledger is in [`COMMON-INTEGRATION.md`](COMMON-INTEGRATION.md).

Feature-level requirements include:

- **FOUNDATION** Exact reviewed Common revision is pinned and reproducible.
- **FOUNDATION** Canonical System/Day/Night design palette from Common.
- **FOUNDATION** Canonical structural metrics from Common.
- **FOUNDATION** Canonical MB Corpo typography identity/provenance from Common.
- **CORE** Canonical file/disk quantity formatting from Common when its contract fits.
- **CORE** Common checked arithmetic for byte counts, sizes, progress and buffer calculations.
- **CORE** Common deterministic string/ASCII primitives for protocol/config identifiers.
- **CORE** Common strict UTF-8 validation/encoding where product-neutral validation is required.
- **CORE** Common localisation engine for product strings once localisation is introduced.
- **CORE** Common project identity/build-profile contract.
- **CORE** Common monotonic timing primitives for operation/UI scheduling where appropriate.
- **CORE** Common XDG/home/path primitives in non-UI POSIX code.
- **CORE** Common exact POSIX I/O and durable publication/removal for Files-owned state/journals/config where its contract fits.
- **ADVANCED** Common dynamic-library binder for optional native providers.
- **CORE** Common system-wide temporal presentation policy should inform displayed timestamps rather than Files inventing an independent clock/calendar preference vocabulary.
- **CORE** Files-specific semantics remain in Files; InfiltratorFS-specific semantics remain in InfiltratorFS. Common is not a dumping ground.

---

# 26. Preferences and personalisation

- **CORE** System/Day/Night theme policy using Common contract.
- **CORE** Canonical typography; product-specific geometry/icons remain Files-owned.
- **CORE** Default view.
- **CORE** Default sort/group behaviour.
- **CORE** Show hidden files preference.
- **CORE** Thumbnail policy and remote-thumbnail limits.
- **CORE** Single/double click activation policy only if supported without introducing ambiguous accidental activation.
- **CORE** Sidebar visibility/width.
- **ADVANCED** Preview pane preference.
- **ADVANCED** Per-folder view memory.
- **CORE** Search/index privacy exclusions.
- **RESEARCH** Semantic retrieval enable/disable/model/privacy controls.
- **CORE** Preferences persist atomically and survive malformed-state recovery with safe defaults.
- **CORE** Settings that belong system-wide remain owned by System Settings/Common rather than duplicated privately.

---

# 27. Keyboard, mouse, touch and accessibility

- **CORE** Complete keyboard navigation.
- **CORE** Predictable Tab/Shift-Tab focus traversal.
- **CORE** Arrow-key browsing.
- **CORE** Enter/Open and F2/Rename style conventional shortcuts where appropriate.
- **CORE** Standard copy/cut/paste shortcuts.
- **CORE** Back/forward keyboard and mouse-button navigation.
- **CORE** Accessible names/roles/states for controls and file rows.
- **CORE** Screen-reader usable directory listing and operation state.
- **CORE** Respect platform text scaling.
- **CORE** Respect reduced-motion preference.
- **CORE** High-contrast/readability qualification.
- **CORE** Focus indicator always visible when keyboard navigation requires it.
- **CORE** Minimum usable hit target sizes.
- **ADVANCED** Touch-friendly interaction where InfiltratorOS hardware profile requires it.
- **CORE** Drag/drop always has a keyboard-accessible equivalent.

---

# 28. Internationalisation and filename correctness

- **CORE** Full UTF-8 filename handling on the portable model boundary.
- **CORE** Do not arbitrarily truncate names below provider/filesystem limits.
- **CORE** Bidirectional text rendered safely and understandably.
- **CORE** Filename display must not hide control/spoofing hazards that could make different names visually indistinguishable.
- **CORE** Localised UI strings through Common's i18n contract when localisation ships.
- **CORE** Locale-sensitive human presentation separated from deterministic protocol/config comparisons.
- **CORE** Byte-exact underlying name retained even when display escaping is necessary.

---

# 29. Security and trust

- **CORE** No silent execution of downloaded/untrusted executable content.
- **CORE** Clear execute/open/view distinction.
- **CORE** Respect platform quarantine/origin metadata where available.
- **CORE** Archive extraction path traversal protection.
- **CORE** Remote credentials delegated to qualified credential storage.
- **CORE** No plaintext password persistence by Files.
- **CORE** Privilege is scoped and temporary.
- **CORE** Provider/plugin ABI validates inputs and version.
- **CORE** Untrusted preview/metadata parsers are isolated or bounded where practical.
- **CORE** Search/semantic indexes respect file permissions and excluded locations.
- **CORE** Search results cannot reveal content from objects the current user cannot access.
- **CORE** Operation confirmation is risk-based, not blanket dialog spam.
- **CORE** Permanent delete, privilege escalation and destructive replace are unmistakable.
- **CORE** The product never claims stronger deletion/integrity/privacy guarantees than it can prove.

---

# 30. Performance and scalability

- **FOUNDATION** Virtualised list widgets.
- **FOUNDATION** Asynchronous directory enumeration.
- **CORE** Large directories remain interactive.
- **CORE** Metadata fetched progressively and in bounded batches.
- **CORE** Thumbnail work prioritised by viewport/visibility.
- **CORE** Background work cancellation.
- **CORE** Directory monitoring coalesces storms rather than repainting per raw event.
- **CORE** No O(n²) behaviour in ordinary large-directory selection/sort/update paths.
- **CORE** Memory use bounded for million-entry and large-tree operations.
- **CORE** Search indexes incremental and rebuildable.
- **CORE** Remote/network concurrency bounded.
- **CORE** Copy engine adapts buffering/concurrency based on source/destination characteristics after measurement rather than hard-coded folklore.
- **ADVANCED** Native-build optimisation consistent with InfiltratorOS release policy where measured beneficial.
- **ADVANCED** PGO/LTO release qualification where workload evidence justifies it.
- **CORE** Performance regressions have reproducible benchmarks rather than subjective “feels slow” acceptance.

---

# 31. Reliability and qualification

- **CORE** Unit tests for toolkit-neutral models.
- **CORE** Operation-engine tests independent of GUI.
- **CORE** Provider contract tests.
- **CORE** Filesystem matrix: InfiltratorFS plus representative ext4/XFS/Btrfs/FAT/exFAT/NTFS and network/provider paths where available.
- **CORE** Unicode/path-length/name-boundary tests.
- **CORE** Permission-denied tests.
- **CORE** Read-only destination tests.
- **CORE** Full-disk/no-space tests.
- **CORE** Source disappears mid-operation.
- **CORE** Destination disappears mid-operation.
- **CORE** Removable device unplug during read/write.
- **CORE** Network disconnect during enumeration/copy.
- **CORE** Application termination during staged copy/move.
- **CORE** Power-loss/durability-oriented qualification for Files-owned journals/state where relevant.
- **CORE** Symlink race/path substitution tests around privileged or destructive operations.
- **CORE** Huge-file and huge-directory tests.
- **CORE** Sparse-file tests.
- **CORE** Reflink/shared-extent tests where supported.
- **IFS** Persistent-ID and history restore qualification against InfiltratorFS.
- **CORE** Accessibility regression checks.
- **CORE** Day/Night/System theme readability checks.
- **CORE** No release marked successful solely because it launches.

---

# 32. InfiltratorOS integration

- **CORE** Installed as the default file manager only after qualification proves the replacement works.
- **CORE** Desktop launcher/application metadata.
- **CORE** File/folder open handling.
- **CORE** System Settings integration for system-owned preferences rather than duplicate controls.
- **CORE** Common design/temporal policy integration.
- **CORE** Removable-media and mount-service integration.
- **CORE** Notifications only for meaningful long-running/background operation state; avoid notification spam.
- **ADVANCED** System-wide search integration using Files' exact/semantic index through a narrow query interface.
- **ADVANCED** Open/save chooser integration is a separate contract; Files should not assume the main window is an appropriate file chooser.
- **DELEGATED** Desktop shell/panel/window-manager behaviour belongs to the shell/window system.
- **DELEGATED** Desktop icon management should only move into Files if InfiltratorOS explicitly chooses Files as that provider after separate design work.

---

# 33. User-facing diagnostics and evidence

- **CORE** Failed operation explains what failed, on which object, and what remains complete/incomplete.
- **CORE** “Unknown” is a valid state; do not synthesize misleading values.
- **CORE** Operation details available without forcing every user to read them.
- **ADVANCED** Copyable technical evidence for support/debugging: operation ID, provider, source/destination URIs, error class, completed counts, not secrets.
- **ADVANCED** Optional diagnostic log with bounded retention and privacy controls.
- **CORE** UI messages use human wording while preserving machine-classifiable error state internally.

---

# 34. Explicit non-features / ownership boundaries

The following are deliberately not general Files responsibilities:

- **DELEGATED** filesystem formatting;
- **DELEGATED** partition-table editing;
- **DELEGATED** defragmentation;
- **DELEGATED** deep scrub/repair administration;
- **DELEGATED** filesystem implementation policy such as block placement/compression decisions;
- **DELEGATED** full backup product;
- **DELEGATED** antivirus engine;
- **DELEGATED** cloud synchronisation engine;
- **DELEGATED** terminal emulator;
- **DELEGATED** text/media/document editor;
- **DELEGATED** desktop environment control centre;
- **DELEGATED** opaque AI agent allowed to mutate the namespace autonomously;
- **DELEGATED** proprietary metadata database required to access ordinary files.

Files may expose links/actions into specialist products and may consume qualified providers, but it should not absorb every adjacent function merely because a file is involved.

---

# 35. Release sequencing

The intended implementation sequence is deliberately smaller than the catalogue:

1. **Browsing foundation** — location model, async enumeration, views, navigation, devices, launch/open, Common integration.
2. **Deterministic mutation engine** — create/rename/copy/move/trash/delete, collision handling, progress, cancellation, recovery evidence.
3. **Mature file-manager parity** — tabs, split view, bookmarks, properties, permissions, thumbnails/previews, remote locations, archives, batch rename.
4. **Retrieval** — exact/content search, saved searches, activity model.
5. **InfiltratorFS advantage** — identity, native history, reflinks, integrity and stronger recovery through stable capability interfaces.
6. **Research features** — semantic/contextual retrieval and workflow surfaces after prototype and privacy/performance qualification.
7. **Deep polish** — accessibility, performance, PGO where justified, edge-case qualification and interaction refinement.

A feature moves earlier only when it is needed to make an earlier layer correct. Visual novelty alone is not sufficient reason to jump the sequence.
