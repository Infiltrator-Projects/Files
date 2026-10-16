# Files — Design Philosophy

## First-principles position

Files is a clean-sheet user-facing file-management project for InfiltratorOS. It starts with the behaviour the product must own, studies standards, current research and mature implementations as evidence, and then chooses the strongest justified design.

The project does not copy another file manager's interaction model merely because it is established, and it does not reject proven mechanisms merely because they are old. Newness, popularity, implementation convenience, toolkit fashion and feature-count parity are not architecture criteria. A design is preferred when it is stronger in correctness, recoverability, responsiveness, clarity, capability, maintainability or user confidence.

The project should feel simple because the engineering is complete, not because capability has been removed.

## Relationship with InfiltratorFS

Files is intended to complement InfiltratorFS, but it is not an InfiltratorFS-only browser.

The governing rule is the same one already recorded by InfiltratorFS ecosystem design:

- applications must not be required to understand private InfiltratorFS internals;
- standard operating-system and filesystem interfaces are preferred when they express the required contract;
- applications may use stronger filesystem capabilities when available;
- applications must remain correct on other filesystems; and
- filesystem policy remains filesystem-owned.

The normal relationship is:

```text
filesystem exposes capability
        ↓
File Manager discovers capability
        ↓
File Manager presents or uses it safely
        ↓
ordinary fallback remains available elsewhere
```

InfiltratorFS remains authoritative for its persistent format, object identity, transaction semantics, allocation, integrity, snapshots/history, compression and placement policy. File Manager must not reach into private on-disk structures or duplicate those policies.

Where InfiltratorFS exposes qualified capabilities, File Manager should be able to make them first-class user experiences. Candidate integrations include:

- persistent object identity that can survive rename or relocation;
- retained historical generations and previous-version browsing/restoration;
- reflink/clone-aware copying;
- sparse-file preservation;
- integrity and health information where a stable read-only interface exists;
- optional storage-intent hints without prescribing physical placement;
- stable capability discovery; and
- stronger durable-publication semantics beneath ordinary file operations.

These capabilities must be optional. A volume formatted as ext4, XFS, Btrfs, NTFS, FAT, exFAT, a network filesystem or another supported filesystem must still behave as an ordinary first-class location.

Administrative functions that belong to InfiltratorFS Manager, Defragmenter or another specialist product should not be copied into the file manager merely because the underlying filesystem can perform them.

## Product question

The product question is:

**How can a person locate, understand, organise, manipulate, share and recover their files with the minimum cognitive effort and the maximum justified confidence?**

That question is broader than directory browsing but narrower than turning the file manager into a general-purpose operating-system control centre.

## Mercedes-Benz excellence as the execution standard

"Mercedes-Benz excellence" is a quality benchmark, not a branding claim or product affiliation.

The useful principles are technical excellence, quality, reliability, safety/trust, effortless comfort, intelligent personalisation, reduction and disciplined visual identity. The interface should combine engineering depth with an apparently effortless surface.

For this project that means:

- **Safety and trust:** file operations make their state, risk, reversibility and result clear.
- **Engineering quality:** operations are correct under interruption, partial failure, slow media, network failure, rename races and hotplug.
- **Effortless comfort:** frequent actions require very little navigation or ceremony.
- **Intelligence:** the product may anticipate, search, relate and suggest, but must not silently take authority away from the user or filesystem.
- **Sensual purity / reduction:** remove visual and interaction noise without removing capability. Advanced depth appears when relevant rather than being permanently exposed.
- **Craftsmanship:** typography, spacing, animation, latency, focus behaviour, keyboard behaviour, drag/drop and error handling are all product quality, not finishing work.

## Core interaction principles

### 1. Navigation, search and semantic retrieval are peers

Research continues to show that many users strongly prefer folder navigation, while a minority rely heavily on search. File Manager therefore must not replace hierarchy with search, or search with hierarchy.

The intended model has at least three equal retrieval paths:

- spatial/hierarchical navigation for users who remember *where*;
- exact/metadata/content search for users who remember *what*; and
- optional semantic/contextual retrieval for users who remember *meaning, relationship or activity*.

Semantic retrieval is additive. It must never redefine the authoritative namespace.

### 2. Files remain real files

The operating-system/filesystem namespace remains authoritative. A semantic index, activity model, recent-items model or relationship graph is a derived accelerator and may always be rebuilt.

No proprietary catalogue may become the only way to find or interpret a user's files.

### 3. Deterministic mutation, intelligent assistance

Intelligence may help find files, explain relationships, prepare an operation plan or suggest likely actions. Actual mutation is performed by a deterministic file-operation engine with explicit preconditions, progress, cancellation rules, failure semantics and post-verification.

An AI or semantic component does not directly improvise destructive filesystem operations.

### 4. Recoverability is a primary feature

Copy, move, rename, replace, delete and batch operations are not considered complete merely because they succeed in the normal case.

The product must design the failure and recovery path at the same time as the success path. Where the filesystem or platform provides history, snapshots, trash, atomic rename or other recovery primitives, File Manager should surface them coherently.

Permanent deletion must be clearly distinct from reversible deletion.

### 5. Object and path are different concepts

A path is a namespace location, not necessarily the durable identity of an object.

File Manager's internal model should therefore be able to represent an object separately from its current path when the platform/filesystem exposes a suitable stable identity. InfiltratorFS persistent 128-bit object identity is the strongest example. Other filesystems may expose weaker identifiers, and a path-only fallback remains necessary.

### 6. Context is useful but must remain explainable

Activity, related-file and semantic surfaces should be able to answer *why* an item appeared: same folder, same project/activity, recently used together, similar content, explicit user collection, shared persistent identity/history, or another concrete signal.

Context should reduce hunting, not create a mysterious second namespace.

### 7. First paint before deep work

The shell should appear before indexing, thumbnail generation, network discovery, semantic processing or slow-volume enumeration completes.

Slow work is asynchronous, cancellable where meaningful, and isolated from the UI thread. A dead SMB server, failing USB device or enormous directory must not freeze unrelated navigation.

### 8. Progressive disclosure rather than permanent complexity

Common actions should be visible and obvious. Powerful metadata, history, integrity, detailed properties, operation diagnostics and specialist actions should be available without occupying the primary surface continuously.

The goal is not to hide capability; it is to reveal the right depth at the right time.

### 9. Capability discovery rather than filesystem-name branching

Prefer asking whether the underlying platform/location supports a capability over hard-coding behaviour solely from a filesystem name.

Examples include clone/reflink, sparse preservation, history, object identity, integrity state, atomic replacement and durable publication.

Filesystem-specific providers are appropriate only where no adequate generic operating-system contract exists.

### 10. Local-first privacy

Metadata and semantic indexing should be local by default. The user's filesystem is private information.

Any future remote model/service path must be explicit, optional and separable from ordinary file management. File operations must not require remote semantic services.

## Browse presentation design brief

Directory presentation is a task tool, not a cosmetic preference and not three independent file managers. Research and established file-management behaviour both indicate that different tasks benefit from different spatial density and information emphasis. Files therefore defines three primary browse presentations around cognitive task rather than competitor terminology:

1. **Detail / analytical view** — one item per row with a restrained icon and comparable metadata. This is the strongest presentation when the user is comparing names, type, size, dates, state or other attributes.
2. **Visual view** — a spatial grid with large icons or thumbnails and the filename beneath or immediately associated with the visual. This is strongest for recognition, media and visually distinctive content.
3. **Compact / dense-scan view** — a small icon with the filename beside it, flowing into multiple columns where space permits. Its purpose is to maximise the number of names that can be scanned at once without reducing the interface to a tiny version of the Visual grid.

Switching presentation must be cheap and immediate because presentation should follow the current task. The selected mode must not change the underlying location, selection, sorting, operation semantics, drag/drop rules, context actions, keyboard commands or filesystem behaviour. All presentations operate over the same application model and selection state.

**View type and density are separate concepts.** Visual presentation may later support a continuous or stepped icon/thumbnail size control. Detail presentation may support row-density or column choices. Compact is a distinct information layout, not merely Visual with smaller icons. The architecture must not encode icon size as though it were the identity of the view.

The primary selector should remain visually restrained and quickly reachable. Symbolic controls are preferred where they remain unambiguous, with tooltips and accessible names; permanent explanatory text is not required merely to expose the feature. The selector must have an obvious active state and work equally well by pointer and keyboard.

The chosen presentation should persist across normal application restarts. Per-location remembered presentation is optional and should only be added if it proves useful without creating surprising state. A global remembered choice is the baseline contract.

Qualification for browse presentation includes more than successful rendering: selection must survive switching, activation must mean the same thing in every view, keyboard navigation and focus must remain deliberate, sorting must remain consistent, large directories must remain virtualised/responsive, Day/Night presentation must be coherent, narrow windows must degrade gracefully, and drag/drop/context operations must not fork into view-specific policy.

This brief supersedes implementation-convenience interpretations such as treating "small icons" as simply a scaled-down icon grid. Prototypes may explore exact spacing, thumbnail size, column flow and control placement, but they must preserve the three task roles and shared-model contract above.

## Platform and language philosophy

The application model should not be defined by GTK widgets, Linux path strings or one desktop environment. The initial InfiltratorOS implementation can use the strongest practical Linux desktop interfaces while preserving toolkit-neutral concepts for location, object, capability, operation, history and search.

C and C++ are equal first-class choices. Platform frameworks and libraries are accepted when their documented contract is stronger than reimplementation. Reusable product-neutral mechanisms should converge on Libraries only when Common is at least as strong as the best local implementation.

## Non-goals

Files is not intended to:

- replace InfiltratorFS Manager or Defragmenter;
- require InfiltratorFS for ordinary operation;
- create a second authoritative semantic filesystem above the real filesystem;
- autonomously reorganise a user's files because an AI model believes it can improve them;
- duplicate specialist disk administration inside the browsing surface;
- reproduce every feature from Nemo, Dolphin, Explorer, Finder or another competitor; or
- accept a weaker architecture merely to reach a first release faster.

## Decision quality

A substantial design change should identify:

1. the user or engineering problem;
2. the alternatives considered;
3. research, standards or implementation evidence;
4. trade-offs and failure modes;
5. compatibility/fallback behaviour; and
6. the validation method.

A capability is complete only when normal behaviour, failure behaviour and required qualification are complete.
