# Files 0.1.62 — Bounded inspection and navigation tranche

Files 0.1.62 provides a cohesive, read-only capability group behind an opt-in inspection panel, keeping right-click menus compact and leaving every filesystem mutation with the existing journalled controllers.

## Delivered interactions

1. Open a terminal in the current native directory.
2. Open a terminal in a selected native subdirectory.
3. Calculate a selected regular file's SHA-256 hash asynchronously.
4. Compare exactly two selected regular files using SHA-256 digests (digest agreement is not represented as byte-for-byte proof).
5. Copy the visible inspection report as text.
6. Read image dimensions from supported local image headers without decoding full image contents on the GTK thread.
7. Inspect native extended-attribute names/counts, with 64 KiB input and 32 displayed-name bounds.
8. Report whether a POSIX extended access ACL is listed; this is not an ACL editor.
9. Calculate recursive logical and allocated directory sizes and item counts with symlink and mount boundaries.
10. Cancel these read-only background operations, with traversal limited to 250,000 entries and 64 levels and explicit refusal of partial or unavailable totals.

The inspector also presents GIO metadata such as modification/creation/access timestamps, owner/group, native mode, link target and filesystem free/capacity/read-only flags when the provider actually supplies them. It does not alter file metadata, install a new privileged service or duplicate the filesystem operation engine.

## Ownership and safety

Inspection workers hold references to their GFile targets, not to live directory-selection state. GTK dialogs are reached only via weak references when asynchronous results complete; destroying a dialog cancels its active worker. Each inspection has one active task at a time. Operations remain selectable only where file type and native-directory capabilities permit them. Terminals launch through GSubprocessLauncher with a native working directory and argument vector, never shell interpolation.

The directory-size result explicitly refuses unsupported allocation metadata instead of substituting zero. It does not traverse symlink targets or separately mounted native filesystems, and it reports depth/item limits or errors rather than a successful partial figure. On remote providers without an allocation contract, allocation is unknown.

## Qualification

The hosted Ubuntu 24.04 build and all 17 CTest executables passed before release intent. GTK interaction coverage exercises inspection metadata, SHA-256, comparison of two distinct files, clipboard report export, directory-size totals, allocation display and context-sensitive visibility. Standard package build, native-installer qualification, exact artifacts and central beta APT observation remain the release gates.

## Deliberate exclusions

This release is not a full EXIF parser, ACL/permission editor, archive engine, recursive search service, remote terminal launcher, semantic index or InfiltratorFS capability provider. Physical Linux Mint appearance and user-device qualification remain separate from headless validation.
