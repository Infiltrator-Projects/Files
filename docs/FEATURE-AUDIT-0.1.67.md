# Files 0.1.67 — source-based feature reconciliation

**Audit baseline:** Files main commit \`d562d11306d71adbecfdc024059fe45301789267\` (24 October 2026), compared with published v0.1.66 commit \`3df5906bc94c6af7aa569bda64fb820469278d53\`. **Requirement authority:** the original v0.1.61 \`docs/FEATURES.md\` catalogue. **Qualification:** the Verify run for baseline commit completed successfully: https://github.com/Infiltrator-Projects/Files/actions/runs/40075635698.

This document is a **code-evidence audit, not a declaration of feature completeness**. Evidence was sought in current GTK/GIO source, core operation modules, tests, previous feature audits, the exact-history GitHub comparison and hosted verification. The catalogue is deliberately larger than the executable. A source path and passing tests establish implementation and automated regression coverage within stated scope, not every hardware/provider or desktop interaction. Any capability not evidenced here must not be silently counted as complete.

## Inventory and interpretation

The authoritative catalogue contains **520 individual bullet requirements**: **26 FOUNDATION**, **342 CORE**, **82 ADVANCED**, **17 RESEARCH**, **31 IFS**, and **22 DELEGATED**. The delegated requirements are ownership boundaries, not missing Files implementations. The counts describe *scope*, not completed features. This audit does not manufacture a project-wide completion percentage: many requirements span providers, failure paths, UX evidence and performance rather than one implementable command.

Use these status meanings:
- **Implemented**: a concrete source action and corresponding behavioural/automated evidence were identified.
- **Bounded**: a real implementation exists, but explicit constraints prevent claiming the widest catalogue interpretation.
- **Partial**: only part of the required workflow exists.
- **Outstanding**: no qualified end-to-end implementation was found in the reviewed current source.
- **Delegated**: the intended authority belongs outside Files.
- **Unverified**: source or tests do not justify a positive implementation claim; not necessarily proof of absence.

## What was already implemented through v0.1.66

These are actual operational surfaces, not speculative planned designs:

| Surface | Evidence in current repository | Scope and qualification |
| --- | --- | --- |
| Browsing, navigation, three views, multiple windows/tabs, cross-window tab movement | \`src/ui/file_manager_window.cpp\`, \`workspace_tabs.cpp\`, \`workspace_folder_views.cpp\`, \`workspace_navigation_test.cpp\` | GTK/GIO directory enumeration and native UI; not a split-view |
| Bookmarks, mounted places, durable per-folder view, normal-restart browsing state | \`workspace_bookmarks.cpp\`, \`workspace_session.cpp\`, \`src/core/mounted_places.cpp\`, workspace tests | URI-based references; no stable inode/object identity guarantees |
| Single/multi-item Copy, Move, Replace/Keep Both/Skip, clipboard and drag/drop, Trash/Permanent Delete and restoration | \`src/core/*operation_engine.cpp\`, \`batch_transfer_engine.cpp\`, \`operation_journal.cpp\`, mutation controllers and engine/safety tests | Strongest qualified native mutation paths; non-native mutation, sparse/ownership metadata and every filesystem/provider are **not** universally supported |
| Preflight, private staging, durability, interruption evidence and failure-safe ownership cleanup | \`destination_ownership.hpp\`, \`transfer_operation_engine.cpp\`, \`recovery_operation_engine.cpp\`, \`operation_journal.cpp\`, safety/journal tests | Cancellable bounded operations, no power-loss or physical-removal matrix proven here |
| Current-folder filtering and recursive/search-content retrieval | \`src/core/search_engine.cpp\`, \`src/ui/recursive_search.cpp\`, \`search_engine_test.cpp\` | Bounded 100,000 entries/500 hits/64 levels; UTF-8 content search only for eligible files <=1 MiB; **no general persistent index** |
| SHA-256, inspection, directory sizes, text/images/binary Quick Look | \`file_inspection.cpp\`, \`quick_preview.cpp\`, \`detail_metadata.cpp\`, interaction tests | Bounded; not general PDF/media rendering, EXIF editor, or unbounded binary viewer |
| Saved queries, derived tags, duplicate groups, private search exclusions | \`search_persistence.cpp\`, \`tag_catalogue.cpp\`, \`search_engine.cpp\`, persistence tests | Saved searches are not automatically maintained smart collections; duplicate scans are limited to eligible <=8 MiB files/500 candidates |
| Visual creation-time sorting, hidden/special and link markers, extension presentation | \`file_manager_window.cpp\`, \`detail_metadata.cpp\`, \`interaction_surface_test.cpp\` | Authoritative GFileInfo name is preserved for mutation; GIO availability determines metadata |
| Temporal/theme integration and separate Common dependency | \`CMakeLists.txt\`, \`src/ui/theme_controller.cpp\`, \`docs/COMMON-INTEGRATION.md\` | Pinned Common ABI/provider; no duplicate date/calendar authority |

Previous catalogue audits are \`docs/FEATURE-AUDIT-0.1.65.md\` (11 bounded complete, 4 partial among 15 scoped entries) and \`docs/FEATURE-AUDIT-0.1.66.md\` (4 further scoped entries, hence 15 qualified in those **two tranches**, not 15 features total).

## The 24 commits after the published v0.1.66 tag: code versus original requirements

The branch is **24 commits ahead of v0.1.66** and shares its ancestry. Source and test files changed; documentation and the version were not advanced during those commits. A new release cannot lawfully reuse the immutable v0.1.66 tag for different bytes.

| Original v0.1.61 catalogue line | Requirement / work | Current source evidence | Audit judgement |
| --- | --- | --- | --- |
| 138 | Pin important **files** as well as folders | \`FileManagerWindow::toggle_bookmark_for_selected\` in \`workspace_bookmarks.cpp\`; action \`pin-selected\`; bookmark load/save and UI | **Bounded**: selected regular files become durable URI bookmarks. Not a persistent object-ID system. |
| 142 | Broken references identified without silent retarget | Async native bookmark metadata probe and visible Missing/Unavailable states in \`workspace_bookmarks.cpp\` | **Bounded**: native misses distinguished from unavailable/unchecked remote URIs; references preserved |
| 206 | Selection by pattern, type, date and size | \`show_selection_pattern_dialog\`, \`select_same_type\` in \`interaction_controller.cpp\`; explicit GTK assertions in \`interaction_surface_test.cpp\` | **Implemented for visible entries**: all four original criteria; not four distinct catalogue features |
| 77 | Restore interrupted browsing after a crash | \`schedule_session_checkpoint\` in \`workspace_session.cpp\`; \`save_session\` and \`restore_previous_session\` | **Partial**: debounced browsing-state snapshots exist and exclude file operations; crash/restart fault-injection acceptance is not demonstrated |
| 82 | Configurable tab/window opening policy | \`interaction_controller.cpp\` and \`file_manager_window.cpp\` settings/action surface | **Bounded**: folder activation preference without extra toolbar chrome |
| 179 | Visually identify broken symlinks | \`qualify_visible_link\`, async follow and NOFOLLOW confirmation in \`file_manager_window.cpp\` | **Bounded**: confirmed NOT_FOUND is labelled; other provider failures remain unclassified rather than guessed |
| 187 | Thumbnail size and remote-file policy | \`ThumbnailCache::bind\`, \`FileManagerWindow::toggle_remote_thumbnails\`, durable icon-size preference | **Bounded**: 48/64/96 px and opt-in remotely read image bytes; no video/PDF provider |
| 401, 663 | Respect private exclusions in activity and indexing | \`privacy_allows_recent_uri\` in \`search_persistence.cpp\`; Recent read/write checks in \`file_manager_window.cpp\` | **Bounded**: applies URI exclusions to current Recent/history and search; no semantic index exists |
| 583 | Derived tag state must be rebuildable/exportable | \`export_tags_tsv\`, \`import_tags_tsv\` in \`tag_catalogue.cpp\`; import UI in \`interaction_controller.cpp\` | **Bounded**: exact checked TSV replacement of healthy derived store; no object-identity reconciliation or merger of renamed paths |
| Existing navigation/interaction catalogue | Optional single-click and mouse back/forward | \`toggle_single_click_activation\` and mouse navigation hookups in current UI | **Implemented option**; do **not** count each gesture as an invented new requirement |

The preceding additions are the ones evidenced by current diff/source. They do **not** prove that all 24 commits each implement a separate feature, nor that all original requirements are fully closed. The full upstream comparison is https://github.com/Infiltrator-Projects/Files/compare/v0.1.66...main (valid at audit baseline).

## Catalogue section-by-section status: evidence and gaps

This is an inventory of **all 35 catalogue sections** so scope does not disappear behind recent incremental releases. It is **not** a sign-off for every one of 520 individual requirement acceptance paths.

| Catalogue section | What current code supports | Still incomplete, unverified or delegated |
| --- | --- | --- |
| 1 Shell/sessions | Windows, tabs, cross-window movement, normal restoration, crash checkpoint | Split pane; crash-injection qualification |
| 2 Navigation/places | Breadcrumbs, URI, places, pinned folders/files, mounted volumes | Resilient collections and universal provider coverage |
| 3 Views | Detail/icon/compact, sort, label/thumbnail controls, Quick Look | Gallery, video/PDF thumbnails, provider plug-ins |
| 4 Selection/direct manipulation | Multi/range, rubber-band, pattern/type/date/size, drag/copy and bounded move | Every cross-window/provider drag target and full drop-feedback parity unverified |
| 5 Create | New folder and empty-file machinery | Templates, full provider naming matrix and sparse-create option |
| 6 Rename | Inline rename and native journalled mutation | Batch rename preview/templates and broad case-normalisation matrix |
| 7 Copy/move/duplicate/link | Single/batch transfer, journal/conflicts, metadata and recovery | Sparse/hardlink/reflink guarantees, ownership preservation, generic remote mutations |
| 8 Trash/recovery | Native Trash, restore, batch/permanent deletion | Every provider's Trash and physical interruption matrix |
| 9 Deterministic operations | Separated engines, staging, journal, typed evidence | Full filesystem matrix, all provider capability endpoints |
| 10 Search/retrieval | Exact/glob/recursive/UTF-8 content, saved names, bounded duplicates | Indexed, large/cross-root, semantic and smart collections |
| 11 Recent/activity | Recent URI/history and privacy exclusion, time-range search | Activity clustering, downloads/origin attribution and comprehensive history review |
| 12 Properties | GIO metadata, hash, ACL presence, links, provider/capacity, directory totals | Editable permissions, rich EXIF/media details and full property editor |
| 13 History/versions | Journal interruption inspection and recoverable native Trash | Filesystem-native snapshots/version browsing and undo history |
| 14 Devices/volumes | GIO mounted/removable discovery and supplied mount identity | Complete mount/unmount/eject integration and surprise-removal hardware qualification |
| 15 Remote/network | GIO browsing and limited image reading | Full remote copy/move/rename/conflict/resume workflow |
| 16 Phones/cameras | Provider locations where GIO supplies them | Qualified MTP/PTP import/transfer/device workflows |
| 17 Archives | None qualified end-to-end | Open, browse, create, extract, secure path validation workflow |
| 18 Launch/open | Default application opening, Open With, native-folder terminal | Complete associations and all remote/permission cases |
| 19 Permissions | Read-only mode, owner/group and ACL-present inspection | chmod/chown/ACL editor and privileged helper |
| 20 Clipboard | Text paths/URIs, file cut/copy/paste and transfers | Every cross-app/provider target and security-context qualification |
| 21 Compare/checksums | SHA-256, content compare and supplied checksum verification | Signatures, broad hash-provider formats, filesystem-native integrity |
| 22 Tags | Derived tag store, search, bulk editing, export/import | Stable-identity migration and native persistent labels |
| 23 Extensions/providers | Existing GIO and Common providers | Pluggable qualified preview/search/action/provider ABI |
| 24 InfiltratorFS | Planned capability consumption boundary | Native identity/history/reflink/integrity provider not implemented |
| 25 Common | Pinned full Common library plus theme/temporal/state policies | Independent cross-product runtime compatibility matrix unverified |
| 26 Preferences | View, theme, sidebar, extension, click, thumbnails and tabs | Complete personalization/preferences contract unverified |
| 27 Accessibility/input | Keyboard actions, mouse navigation and GTK focus facilities | Physical keyboard, screen-reader, scale and high-contrast audit |
| 28 Internationalisation/names | Exact GFileInfo names for underlying operations | Full bidi/spoofing, unusual Unicode and localization qualification |
| 29 Security | Fail-closed privacy, bounded workers, guarded native mutations | Cross-provider privilege, malicious archive/preview/input matrix |
| 30 Performance | Async GIO, cache, bounded search and operations | Million-entry benchmarks, network stress and measured indexing |
| 31 Reliability | 20 hosted CTest executables, mutation safety tests | Multi-filesystem/hardware removal, physical crash/power loss tests |
| 32 OS integration | Desktop entry, Common styling and package integration | Default-manager replacement qualification and all system interfaces |
| 33 Diagnostics | Structured failure evidence, selectable inspection results | Full optional diagnostic retention/report tooling |
| 34 Deliberate non-features | Formatting, defrag, repair, backup, antivirus, terminal/editor delegated | Correctly **not** implemented in Files itself |
| 35 Sequencing | Foundational and selected retrieval slices shipped | Full intended parity/IFS/research plan still ahead |

## Verification and remaining gates

1. The exact previous main SHA \`d562d113...\` passed [Verify #465](https://github.com/Infiltrator-Projects/Files/actions/runs/40075635698), including build/test/package and native-installer qualification. That is strong automated **engineering evidence**, not live Mint visual acceptance.
2. At audit time the GitHub **published** v0.1.66 tag was \`3df5906...\`, while main was 24 commits ahead. Green CI **did not imply publication**: the workflow release-intent rule intentionally excludes an existing version whose tag identifies older bytes.
3. A new **v0.1.67** tag must point at the exact new tested commit, ship a \`.deb\`, native installer and \`SHA256SUMS\`, and appear in the central beta APT package index **with matching Debian SHA-256**, and catalogue version. Only then is publication complete.
4. Live Linux Mint Day/Night UI/accessibility review, hardware/device/network failure injection, broad filesystem tests and true end-to-end provider/IFS qualification **remain outstanding**.
5. The original authoritative catalogue is **not narrowed** by this report. To calculate an auditable completed/remaining count, each of its 520 entries needs its own acceptance/evidence ledger and treatment of delegated and research-only scope, rather than counting source functions or UI actions.

**Conclusion:** substantial ordinary file-manager implementation and additional verified source capabilities are real; **full feature completeness is false**. Treat v0.1.67 as a new bounded source release, never as the end of the feature catalogue.
