# Files 0.1.75 — ten documented catalogue slices

**Baseline:** merged [PR #22](https://github.com/Infiltrator-Projects/Files/pull/22), exact source integration commit `2dab5c3577223474b06a2230d1a09b1efcbc49b9` before the release version/notes commit.

This candidate advances **ten distinct entries from the original feature catalogue**, traced by original line number and implementation/qualification boundaries in [the ten-feature audit](FEATURE-AUDIT-NEXT-TEN-2026-10-27.md). It does not declare all ten universally complete: immediate inline rename is optional, received-file origins are inspection-only, archive preview is bounded, and the permission entry hardens previously existing functionality. See the audit's remaining gates.

## Operational changes

- User Templates can supply a selected native file for creation via the existing conflict-safe, journalled copy engine.
- Native new-file/folder/sparse-file dialogs offer post-create inline rename when the row is available; oversized/unavailable source data fails visibly rather than creating unsafe output.
- Symbolic/hard links are created with no-replace destination semantics, inode/no-follow constraints, explicit unsupported states and journal lifetime guards.
- Sparse file creation offers explicit size within a 1 TiB logical limit; it does not promise physical allocation semantics unsupported by the filesystem.
- Recent native file opens are tracked separately from recent locations, bounded to 20 items and subject to existing privacy exclusions.
- File inspection displays supported origin/referrer extended attributes only when present.
- Read-only archive preview reports bounded header names, entry counts, claimed sizes and unsupported/unsafe entries; it does not extract or execute data.
- Mode-000 owner permission recovery uses held-descriptor Linux fchmodat2 where supported and refuses unsafe pathname fallback.

## Exact release gate

The feature candidate head `597c9743709150267a081829b3bf8fcacde03243` passed GitHub [Verify run 40593454352](https://github.com/Infiltrator-Projects/Files/actions/runs/40593454352), including 22/22 main tests, Debian packaging and the native installer. The **new 0.1.75 versioned commit** must independently pass exact-head Verify before its GitHub tag/assets and central beta APT publication can be considered complete. Release-intent must not bypass any gate.

The application remains independent at `/Programs/files`. Nemo remains installed and remains the default on Linux Mint until Files receives explicit full replacement acceptance. Full physical UI/accessibility, remote/MTP hardware, interruption and wider catalogue testing are not claimed.
