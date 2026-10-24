# Files 0.1.65 — Original catalogue traceability and qualification

**Canonical source:** [Original 0.1.61 feature catalogue](https://github.com/Infiltrator-Projects/Files/blob/v0.1.61/docs/FEATURES.md). The requirements below refer to **original numbered lines**, not new subfeatures invented for release bookkeeping. Fifteen separate catalogue entries are in scope. A bounded working subset does not automatically close a broader entry.

| Original line | Original requirement | Honest status | Delivery and qualification |
|---|---|---|---|
| 357 | Search selected locations | **Bounded complete** | Search 2–16 selected folders in one opt-in search, aggregate bounded results; GTK interaction test. Cross-root duplicate grouping refused. |
| 359 | Metadata filters (type/size/dates/owner/tags/collections/location) | **Partial** | Adds owner UID and user-tag criteria to existing type/size/date and location scoping; saved collections filtering remains missing. |
| 365 | Saved searches / smart collections | **Partial** | Up to 16 named queries, saved atomically with strict loading and scoped URI; no automatically maintained smart collections. |
| 366 | Duplicate/content-hash search | **Partial** | SHA-256 grouping of bounded regular files up to 8 MiB and 500 candidates in one directory tree. Larger files and cross-root groups are not represented as complete. |
| 397 | Recently modified by time range | **Bounded complete** | Inclusive local start and exclusive local end, rejects inconsistent intervals and unknown provider timestamps. |
| 421 | Symbolic-link target and link status | **Complete when GIO can resolve** | Distinguishes available target, confirmed broken target, and unknown/unreadable. Never follows for mutations. |
| 422 | Filesystem/volume/provider | **Complete when provided** | Shows GIO provider scheme, filesystem type, qualified mount name and available capacity/read-only attributes; unavailable stays unknown. |
| 568 | Compare selected files by size/hash/content | **Bounded complete** | Reports sizes, SHA-256 digests and exact streamed byte comparison for matching digests. Concurrent writes cannot yield a snapshot guarantee. |
| 569 | Verify downloaded/provided checksum text | **Bounded complete** | Supports SHA-256 hex or standard single-line sha256sum record that names the selected file exactly, explicitly identifies mismatch. No signature/trust guarantee. |
| 578 | User tags/labels without renaming | **Bounded complete** | Private derived URI-to-label catalogue, no filename/content mutation, max 512 records. |
| 579 | Search/filter by tag | **Bounded complete** | Exact URI tag matches bound to user-authored query, no path-prefix heuristics. |
| 580 | Tag multiple selected objects | **Bounded complete** | 1–128 selected items, at most 16 validated labels per item; atomic file persistence. |
| 581 | Native stable metadata vs derived catalogue separation | **Bounded complete** | Explicitly uses a separate derived URI reference map rather than modifying xattrs, inode metadata or authoritative namespace state; path identity is weak and may become stale. |
| 583 | Derived tag database rebuildable/exportable, nonessential | **Partial** | Readable private state and copyable TSV export; deleting it does not affect browsing. Direct TSV re-import and identity-preserving reconciliation are not yet delivered. |
| 663 | Search/index privacy exclusions | **Complete for existing search** | Up to 64 persisted excluded roots, honoured by every current GIO scan; corrupt privacy state disables search instead of silently dropping exclusions. No general persistent semantic index exists yet. |

## Product impact

- **Golden journeys:** GJ-04 retrieval from remembered fragments; GJ-06 safe file inspection and organisation; GJ-09 failure and recovery evidence.
- **User-visible improvement:** search owner/date filters and tags, find bounded duplicates, verify checksums, inspect links/provider context, save named searches and private exclusions.
- **Engineering ownership:** `search_engine` owns bounded GIO enumeration and hashes; `tag_catalogue` owns derived labels; `search_persistence` owns named queries and privacy state through Common-backed `presentation_store`. `InteractionController` delegates UX; only existing operation controllers own mutations.
- **Failure/recovery:** Corrupt tag, saved-search or exclusion data is preserved; malformed exclusions fail closed. Remote and unknown metadata does not become invented zero/clean status. Search limits, unsupported multi-root duplicate mode and skipped files are surfaced.
- **What remains unproven:** actual Linux Mint Day/Night visual/keyboard walkthrough; large remote/mount-stress performance; multi-process simultaneous edits to derived catalogue; stable tag identity after external rename or relocation; automatic smart collections; unrestricted duplicate scan; direct TSV re-import.
- **Verdict:** Meaningful functional progress with recorded limitations, **not** 15 fully retired catalogue requirements. **11 scoped complete, 4 partial.**

## Qualification evidence

Hosted Ubuntu 24.04 release source build and 20 CTest executables exercise core search, real GTK interactions, tag persistence, saved-search persistence and fail-closed privacy settings. Native installer and publication of the exact package remain mandatory gates, as does a separate physical Mint UI acceptance step.

No successful release is inferred from a planned feature, a commit, or a green compilation alone.
