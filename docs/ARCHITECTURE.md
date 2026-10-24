# Files — Initial Architecture

This document defines the architectural model derived from the project's design philosophy and October 2026 research review. The early C++20/GTK 4 implementation now provides browsing, mounted places, deterministic operation engines and an append-only operation journal. The larger object/capability, retrieval and provider models below remain intended architecture; their presence here does not claim implementation. Performance measurements, prototypes and further research may change details before code architecture is frozen.

## 1. Architectural boundary

Files owns the user-facing model of files, locations, retrieval, operations and recoverability.

It does **not** own filesystem allocation, persistent on-disk structures, filesystem transaction semantics or specialist disk administration. Those remain responsibilities of the operating system and filesystem.

The architecture separates four kinds of truth:

1. **filesystem truth** — the actual namespace, objects, metadata and supported storage semantics;
2. **application model** — File Manager's current representation of locations, objects, capabilities and operations;
3. **derived indexes** — search, semantic and activity/context indexes that can be discarded and rebuilt; and
4. **presentation state** — windows, selection, sorting, layout, expansion and other UI state.

Only the first two may govern file mutation. Derived indexes and presentation state never become authoritative storage.

## 2. Layered model

```text
┌───────────────────────────────────────────────────────────────┐
│ Presentation                                                 │
│ browse / search / semantic / context / history / operations  │
└───────────────────────────┬───────────────────────────────────┘
                            ↓
┌───────────────────────────────────────────────────────────────┐
│ Application model                                             │
│ Location • FileObject • Container • CapabilitySet            │
│ SearchHit • HistoryEntry • ActivityContext • OperationState  │
└───────────────────────────┬───────────────────────────────────┘
                            ↓
┌───────────────────────────────────────────────────────────────┐
│ Deterministic operation engine                                │
│ preflight • plan • execute • durability • verify • recover   │
└───────────────────────────┬───────────────────────────────────┘
                            ↓
┌───────────────────────────┴───────────────────────────────────┐
│ Platform/filesystem capability layer                          │
│ standard OS APIs • generic capabilities • provider extensions│
└──────────────┬───────────────────────────────┬────────────────┘
               ↓                               ↓
       ordinary filesystems             InfiltratorFS provider
       remote locations                 qualified extra features

Derived side systems:

filesystem events ──→ exact/content index ──┐
                                            ├─→ retrieval model
filesystem events ──→ semantic/context index┘

Both indexes are rebuildable and non-authoritative.
```

## 3. Core model

The initial model should be expressed in toolkit-neutral concepts rather than GTK objects.

### Location

A browseable namespace endpoint or container context.

Examples:

- local directory;
- mounted volume;
- remote share/location;
- trash;
- search result space;
- history view;
- explicit collection; or
- derived activity/context view.

A derived location does not imply a new filesystem namespace.

### FileObject

Represents one logical item known to the application.

Potential fields include:

- current location/name;
- object kind;
- metadata and availability flags;
- stable identity when available;
- current capability set;
- content type;
- size/logical size/physical size where available;
- timestamps;
- integrity/history indicators where available; and
- source/provider identity.

Unknown and unavailable values remain distinct from zero/empty values.

### CapabilitySet

A per-location or per-object description of supported behaviour.

Candidate capabilities include:

- read/write;
- atomic rename/replace;
- trash/reversible deletion;
- clone/reflink;
- sparse preservation;
- xattrs/named metadata;
- stable object identity;
- historical-version enumeration;
- historical restore;
- integrity status;
- storage-intent hints;
- durable publication guarantees; and
- remote/offline semantics.

The UI should branch primarily on capability, not on string comparisons such as `filesystem == "infiltratorfs"`.

### HistoryEntry

Represents a previous state or recoverable operation in a provider-neutral form.

History sources may include:

- filesystem-retained generations;
- trash;
- application operation journal;
- platform versioning facilities; or
- other qualified providers.

The UI must show where a history item comes from and what restoration means.

### ActivityContext

A derived grouping of files related by user activity, explicit grouping, chronology or other explainable signals.

ActivityContext is never filesystem truth and must not silently move files.

## 4. Identity model

Paths and object identity are deliberately separate.

Preferred identity order:

1. qualified persistent filesystem object identity, where exposed;
2. a platform/filesystem stable identifier with documented lifetime;
3. a compound weaker identity valid only inside a known mount/session; or
4. path-based fallback.

InfiltratorFS persistent 128-bit IDs can provide the strongest form when a stable public capability interface exists.

Derived indexes must record the strength/scope of the identity they rely on. They must tolerate rename, move, remount, replacement and identity invalidation rather than assuming pathname permanence.

## 5. Operation engine

All mutating filesystem actions pass through one deterministic operation engine.

A normal operation has the conceptual phases:

```text
request
   ↓
resolve objects/locations
   ↓
preflight capabilities + conflicts + permissions + space
   ↓
build explicit operation plan
   ↓
execute
   ↓
establish required durability where supported/required
   ↓
post-verify result
   ↓
publish final state + recovery/undo information
```

The engine owns:

- copy;
- move;
- rename;
- replace/overwrite;
- create;
- delete/trash;
- restore;
- batch operations; and
- provider-backed clone/history operations.

Semantic/AI systems may create candidate selections or proposed plans but cannot bypass the operation engine.

### Failure states

At minimum, operations distinguish:

- success;
- partial success;
- cancelled cleanly;
- cancelled with retained partial result;
- permission failure;
- source disappeared/changed;
- destination conflict;
- insufficient space;
- destination disconnected;
- unsupported capability;
- durability could not be established; and
- verification failure.

The UI should not collapse these into a generic "something went wrong" state.

### Native copy durability and metadata

`copied_metadata` owns preservation of native regular-file/directory modes, modification times, enumerated extended attributes and POSIX ACLs on identity-bound private output descriptors. Non-ACL attributes are applied while outputs remain privately writable; ACLs are applied after chmod, with destination-only inherited ACLs removed. Linux attribute lists/values are bounded at 64 KiB and aggregate values at 4 MiB per object. Unreadable, unsupported, changing or oversized metadata fails the transfer explicitly. Symlinks with extended attributes are refused because Linux cannot apply them through an identity-bound O_PATH descriptor without following a separately observed pathname. Ownership and sparse allocation remain separate future policy.

Copied file data and metadata, child directories and the private staging directory are flushed before atomic publication. The public destination parent is then flushed before a cross-volume move can quarantine/remove its original source. A pre-publication failure uses the existing exact retained-evidence path; a publication-barrier failure leaves the source intact and identifies the retained public copy. Source-parent synchronization follows successful source removal. A flush failure is a verification failure, never successful completion inferred from cached read-back. Cancellation is checked again after private durability and before publication.

These barriers do not create snapshots of actively written source/destination objects, guarantee hardware honesty, or replace real-device power-loss qualification.

## 6. Recoverability and journal

File Manager should maintain a user-visible operation history sufficient to explain recent mutations and expose reversible actions when they genuinely exist.

The operation journal is not a substitute for filesystem history. It records what File Manager attempted and established.

A journal entry may include:

- action and timestamp;
- source/destination identities and paths;
- capability path used (ordinary copy, reflink, atomic replace, trash, etc.);
- result state;
- verification state; and
- an undo/restore token only where an actual reversible mechanism exists.

The application must never manufacture an Undo button when no reliable reversal exists.

Recovery scanning and append admission share one complete-frame validator. Under the process mutex and journal flock, malformed complete records block START/END appends before bytes are changed; incomplete final frames retain the established repair contract. A single bounded in-process validation checkpoint avoids quadratic rescanning during large batches. It is invalidated by any observed inode, size, mtime or ctime change and is never a separate persistent authority. Cooperating writers must honor the advisory lock; this is not continuous detection of arbitrary non-cooperating or physical storage corruption.

## 7. Search architecture

### 0.1.65 retrieval and derived tags

The GIO search engine remains the sole read-only enumerator; it implements bounded per-root duplicate digest grouping, owner/time criteria and immutable exact-URI label membership without any mutation authority. A selected-root search aggregates only bounded ordinary search results; cross-root duplicate comparison is explicitly refused rather than giving an incomplete positive verdict. Permanent search privacy exclusions are kept independently from named presets; an invalid exclusion file disables scanning rather than falling back to an empty allowlist.

`tag_catalogue` owns the private derived URI/label association file. It is not a filesystem or inode identity service: external moves and renames may invalidate labels, and the durable map is never consulted by mutating operation preflight. `search_persistence` owns versioned, bounded named presets and mandatory private search exclusions, using Common-backed private atomic storage through `presentation_store`. A failed or malformed load preserves the source bytes, distinguishes unavailable state and refuses conflicting writes.


Search has independent layers:

### Exact/native search

Uses filenames, paths, metadata and optionally indexed content with deterministic query semantics.

### Semantic search

Uses a rebuildable local semantic index. It may index supported document content, metadata and derived context.

Semantic results must preserve:

- source object identity/path;
- confidence/ranking information;
- an explanation or provenance signal where practical; and
- a route back to the actual filesystem object.

Semantic search must degrade to unavailable without affecting ordinary browsing/search.

### Context retrieval

May combine explicit and derived relationships such as:

- same folder;
- recently used together;
- same activity/project;
- related content;
- shared historical lineage; or
- explicit collection membership.

The system should distinguish inferred relationships from user-authored ones.

## 8. InfiltratorFS capability provider

The InfiltratorFS integration is an optional provider behind the same application model.

It may expose qualified capabilities that generic OS APIs cannot represent strongly enough, for example:

- persistent 128-bit object ID;
- retained-generation enumeration;
- object-level historical open/restore when implemented and qualified;
- integrity status;
- richer clone/shared-data information;
- filesystem-specific durable-publication guarantees; and
- versioned storage-intent hints.

The provider must use a stable public interface. File Manager must not parse InfiltratorFS private metadata or on-disk structures.

If the provider is missing or the filesystem is not InfiltratorFS, the same application model uses generic platform capabilities.

## 9. UI execution model

The GUI is first-paint oriented.

Before the first visible frame, only work required to show a useful shell/current location should run synchronously.

The following are asynchronous by default:

- directory enrichment beyond minimum enumeration;
- thumbnail generation;
- content indexing;
- semantic indexing;
- network discovery;
- remote metadata probes;
- history enrichment;
- integrity queries that may block;
- related-item computation; and
- large property aggregation.

Results merge incrementally into visible models using stable identities/generations so late worker results cannot corrupt a newly selected location.

A failed background subsystem must fail open: the normal file manager remains usable.

### Mutation, monitoring and application lifetime

`DirectoryMutationGuard` provides the single UI-thread lease for each window's directory model. Creation, single/batch operations, destructive/recovery and clipboard controllers suspend the same native monitor before dispatching worker mutation. Overlapping operations increment one shared count; only the last completion can re-enable monitoring, which causes GTK to enumerate the resulting directory again. Removal from the GtkApplication permanently revokes UI authority and disables monitoring before GTK unrealizes the window; the later widget destroy signal supplies the disposal fallback. This distinction matters because pending GTasks can delay widget destruction. Results are still consumed and journalled without touching disposed controls. Pending clipboard/chooser completions and progress timers honor the same lifetime boundary.

The first active lease holds the GtkApplication, and the last releases it after worker completion. Admission returns an explicit boolean: every controller must acquire it before worker dispatch and release it after failed journal setup. A queued clipboard conflict result cannot restart mutation after window removal. A reference to a destroyed GtkWindow alone does not authorize the main loop to exit before outstanding mutation/journal work finishes. Browsing remains available during worker execution, and presentation remains separate from durable operation semantics.

## 10. Presentation model

### Quick Look preview authority (0.1.64)

`quick_preview` owns only the ephemeral read-only preview session. It snapshots at most 4,096 visible regular-file GFile references so a sort, navigation or selection change cannot retarget an in-progress read. Background GIO reads and image decoding carry per-request cancellables, monotonically increasing generations and weak GTK session references. A late completion can neither resurrect a destroyed preview nor overwrite a newer request. It exposes no mutation commands and consumes no operation-journal lease. For unknown-size or >8 MiB objects it visibly refuses content reads; text and hex rendering are independently bounded. Persistent preview caches or user-visible document/image/media provider contracts are deliberately not introduced by this tranche.


### Workspace persistence and interaction ownership

`workspace_tabs`, `workspace_session` and `workspace_bookmarks` implement cohesive
presentation responsibilities over the existing window model. Typed in-process
drag payloads carry stable tab/bookmark identities and weak window references;
closed windows or removed entries revoke their authority. Tab movement retains
the existing history, selection and view state rather than reinterpreting files.

`presentation_store` uses Common's bounded text reader and private atomic writer
for versioned presentation files (1 MiB maximum). Session restore validates the
entire input before replacing tabs, bounds histories/selections and enumerates
only the active location asynchronously. Normal shutdown saves the last window's
workspace; multiple windows and in-flight operations are not reconstructed.
Bookmark edits commit one durable file before other open windows reload it;
unreadable existing bookmark state disables writes rather than overwriting it.

`TransferProgressView` owns only progress widgets and a UI timer. Controllers
retain worker, cancellation-control and mutation-admission ownership. Replacement
and tree measurement consume the same cancellation token as ordinary transfer.
`BatchRemovalEngine` preflights distinct native siblings, then invokes the existing
recovery engine under a separate durable START/END for each item. Cancellation
between items reports the completed count; an individual recursive delete remains
indivisible for cancellation and retains its existing quarantine/recovery contract.

The production UI should be selected after prototyping, not inferred directly from current file managers.

The architecture nevertheless assumes several information roles:

- **primary browse/search canvas** — files and containers;
- **navigation context** — hierarchy, history and locations;
- **zero-layer contextual actions** — a restrained set of actions relevant now;
- **detail/inspection surface** — progressively disclosed metadata/capabilities/history;
- **operation surface** — progress, conflicts, errors and recovery; and
- **retrieval mode** — exact and semantic search without losing browse context.

UI concepts must map to the same underlying application model rather than implementing independent file logic per view.

### Browse presentation invariants

Directory view mode is presentation state over one shared browse model. The initial production contract distinguishes three task-oriented presentations: **Detail / analytical**, **Visual**, and **Compact / dense scan**.

The presentation layer may create different GTK/view objects to render those modes efficiently, but it must not duplicate navigation, selection authority, sorting policy, activation behaviour, drag/drop policy, context-action policy or file-operation logic. A switch of presentation must preserve the user's logical selection and current location and must not trigger a filesystem mutation or reinterpret the underlying objects.

View identity is separate from density. A Visual view may support different icon/thumbnail sizes without becoming a different semantic view. Detail may vary row density or columns. Compact remains a distinct multi-column small-icon-plus-label composition rather than a shrunken Visual grid.

The remembered presentation is application-level presentation state. Global persistence across ordinary restart is the baseline. Per-location presentation memory may be layered on later, but must not be required by the browse model and must not create hidden filesystem semantics.

All three presentations must consume the same asynchronous directory data and remain compatible with large-directory virtualisation. Switching presentation should not cause duplicate enumeration or independent metadata pipelines unless measurement proves a view-specific enrichment is required.

## 11. Security and privacy

File content, paths, filenames, metadata, semantic vectors and activity relationships are private user information.

Initial rules:

- local indexing by default;
- no network requirement for normal file operations;
- least-privilege execution;
- privilege elevation only for the narrow action that requires it;
- clear boundary between browsing and destructive/admin actions;
- provider outputs treated as untrusted until validated against contract; and
- no direct semantic-agent filesystem mutation.

## 12. Qualification model

The file manager must be tested against real failure, not only correct local files.

Qualification should eventually include:

- enormous directories;
- deep directory trees;
- long valid UTF-8 names;
- hard links and symlinks;
- sparse files;
- reflink-capable and non-reflink filesystems;
- removable-media removal during enumeration and transfer;
- remote filesystem latency/disconnect/reconnect;
- permission changes during an operation;
- source replacement/rename races;
- low/free-space exhaustion;
- partial copy/move recovery;
- cancel at each operation phase;
- history/restore correctness;
- search-index corruption/rebuild;
- semantic subsystem unavailable/corrupt;
- mixed filesystems in one operation;
- InfiltratorFS history/object identity integration;
- view switching with preserved selection/sort/location state;
- large-directory behaviour in Detail, Visual and Compact presentations;
- narrow-window and Day/Night rendering across presentations; and
- accessibility/keyboard-only workflows.

The success path, failure path and recovery path are all part of the feature.

## 13. Architecture questions deliberately left open

The initial desktop uses GTK 4/GIO with C++20 Files components and the pinned C Common substrate. The current journal uses escaped tab-separated START/END records; cross-filesystem moves copy, verify and then remove the source. Those choices still need stronger concurrent-writer, crash and namespace-race qualification.

The following remain open and must be decided through research/prototyping rather than assumption:

- further extraction of toolkit-neutral file/object/capability boundaries;
- exact local semantic embedding/index implementation;
- whether activity contexts are automatic, explicit or hybrid;
- stronger operation-journal persistence and cross-filesystem move recovery guarantees;
- remote filesystem abstraction boundaries;
- plugin/extension model;
- preview/thumbnail sandboxing; and
- exact public InfiltratorFS capability API.

## Standalone shared typography resources

Files pins Libraries/Common 1.19.66 and packages the small `InfiltratrEnsureExtendedFonts.sh` helper at `/usr/lib/files/ensure-extended-fonts`. Its Debian configure hook verifies or establishes all 23 shared MB Corpo/Daimler faces at `/System/Resources/Fonts` from the immutable InfiltratorOS font source; fonts are not owned by Files' package and are not removed with Files. The GTK4 theme chooses Text, Condensed and Title families by product task and uses Common's existing fallback contract on systems where the optional extended cache is unavailable at runtime. No file manager operation, filesystem identity or recovery policy depends on a typography resource.
