# Files — dual-pane source qualification (after v0.1.67)

**Source commit:** `efebc19d51aed56c45e341fc6f536efceb707f90`.
The established 0.1.67 release remains immutable; this work requires a new qualified
version before it can be represented as a published feature.

## Implemented contract

- The active FileManagerWindow retains the authoritative primary model and all existing operation controllers.
- A separately-owned `GtkDirectoryList` and single-selection model browse the second pane asynchronously, including valid GIO URI locations. Navigating the second pane does not retarget the first.
- Explicit directional Copy and Move actions capture selected native paths from the corresponding model, never a visually sorted index from the raw primary directory model. A maximum of 256 selected primary entries may be transferred in one interaction.
- Only native destinations are admitted. The secondary controller invokes `ClipboardController::transfer_from_drop`, reusing `BatchTransferEngine` conflict decisions, journal, cancellation and recovery semantics. No second mutation engine, direct file deletion or implicit hover action was introduced.
- Closing the secondary pane relinquishes its enumeration target. Its presentation and owned model references are released with the hosting window.

## Boundaries and remaining work

This is an initial dual-pane workflow, not a claim of the catalogue's broad drag/drop
and per-provider parity. The second pane currently offers a single-selection list,
simple backward navigation and editable URI entry. It has no independent tab set,
thumbnail/grid modes, drag/drop target parity or deep session persistence.
Remote GIO URI browsing is supported where the platform provider makes it available,
but remote copy/move/rename and MTP/PTP imports remain explicitly unqualified.
Moving items relies on the existing native-only operation engine and is not a
standalone source deletion.

The archive creation/extraction workflow, permission/ACL editor and full
remote/portable-device transfer qualification remain separate catalogue work.

## Evidence

The existing `workspace-features` CTest exercises second-pane creation,
visibility, independent URI navigation, collapse and a native cross-pane Copy
against a temporary filesystem root. The CI build and packaging pipeline
provide compile and static acceptance evidence. It does not constitute a
real-device power-loss matrix, remote-provider certification or visual Mint
Day/Night acceptance.

## Follow-up workspace qualification

The secondary list now uses multi-selection for bounded directional native transfers (the same maximum of 256 source items as the primary pane). It supplies explicit Select all and independent Back/Forward history, with forward history invalidated by a new navigation. Both transfer directions still invoke ClipboardController and the shared journalled batch engine; no remote mutations are admitted. History and the secondary browse target remain intentionally in-window only and are released on collapse.
