# Files 0.1.66 — Fifteen distinct original requirements completed in qualified scopes

**Frozen authority:** [Original v0.1.61 catalogue](https://github.com/Infiltrator-Projects/Files/blob/v0.1.61/docs/FEATURES.md). Each source line below identifies a separate original bullet. The release notes do not count controls or implementation steps as extra catalogue requirements.

## Eleven scoped-complete requirements qualified in v0.1.65

See [the complete 0.1.65 audit](FEATURE-AUDIT-0.1.65.md) for precise resource limits, interaction and failure behaviour. Original catalogue lines:

| Original line | Catalogue capability | Qualification |
|---|---|---|
| 357 | Search selected locations | Search 2–16 selected roots within explicit bounds |
| 397 | Recently modified by time range | Inclusive/exclusive local date range filtering |
| 421 | Symbolic-link target and link status | GIO-resolvable status, including genuine unknown |
| 422 | Filesystem/volume/provider properties | Available provider scheme, volume and filesystem metadata |
| 568 | Compare selected files by size/hash/content | Bounded exact byte-stream comparison and SHA-256 evidence |
| 569 | Verify downloaded/provided checksum text | SHA-256 or filename-bound sha256sum input |
| 578 | User tags/labels without renaming | Durable derived URI-to-label mapping |
| 579 | Search/filter by tag | Exact URI-to-tag matching |
| 580 | Tag multiple selected objects | Atomically tag 1–128 selected references |
| 581 | Native vs derived metadata separation | Derived catalogue never becomes filesystem namespace authority |
| 663 | Search/index privacy exclusions | Persisted excluded roots, fail-closed on unreadable state |

Every listed completion has the bounded conditions documented in 0.1.65. A scoped-complete entry is **not** a universal promise covering unsupported providers or unbounded data sets.

## Four additional original catalogue requirements qualified in v0.1.66

| Original line | Catalogue capability | Implemented contract |
|---|---|---|
| 165 | **CORE** Sort by created time when exposed | New Date Created column with native GTK ascending/descending sorting, raw `time::created` comparator, unknown timestamps represented as unavailable, temporal formatting through Common |
| 181 | **CORE** Distinguish hidden, system/special, ordinary entries | Visible hidden marker and special/mountable marker based on GIO attributes, ordinary entries remain unmarked; unknown provider metadata is not guessed |
| 182 | **CORE** Optional display of file extensions without falsifying real name | Explicit Show/Hide filename extensions command and persistent preference; full real name remains in tooltips, raw GFileInfo identity drives sort, selection and mutations, labels are refreshed without changing selection |
| 183 | **CORE** Visually identify symlinks and shortcuts | A distinct visible directional marker appears wherever GIO exposes symlink/shortcut identity, independent of filename extension choice; no link traversal or retargeting |

**All four are independent original requirements, not four controls of one larger feature.** The separate original requirement at line 184, *broken link visually identifiable*, remains open: this release does not infer brokenness without an authoritative target probe.

## Four partial original catalogue entries remain partial, not part of the fifteen completed

- Line 359: full metadata filter family (collection criteria are incomplete).
- Line 365: saved searches are present, automatically maintained smart collections are not.
- Line 366: bounded duplicate grouping exists, full large/cross-root duplicate search remains missing.
- Line 583: derived tag catalogue is exportable but direct reconstruction/re-import remains incomplete.

The full backlog also includes many other distinct catalogue entries. This audit is not a claim that Files itself is feature complete.

## Engineering and qualification

Implementation authority remains unchanged: `detail_metadata` owns metadata presentation/comparison; the window owns the GtkColumnView and visible-row policy; the interaction controller delegates menu command execution; the original filesystem mutation controllers alone own writes to user files. The extension switch cannot redirect actions because the pointer picker now matches `GFileInfo::name` from each realised item's `ifm-source-name`, never the rendered label. The persisted extension preference is display-only and never transforms the underlying filename.

The exact source changes passed **20 of 20 hosted CTest tests** and the Debian/native-installer qualification before release-intent. Tests include creation-time sorting and absent timestamps, extension preservation, symlink/hidden/special labels, action state and selection stability. Final release and central APT digest verification are separate publication gates.

**Outstanding evidence:** actual Linux Mint Day/Night visual and accessibility qualification; ambiguous displayed stems with extensions hidden deserve live UX review even though operations use byte-exact source identities. No physical crash/failure testing is inferred from headless CI.
