# Files — next ten catalogue requirements (27 October 2026)

**Authority:** original [520-line requirement catalogue](FEATURES.md), unchanged; the entries below refer to the **original source line numbers**. **Baseline:** published 0.1.73 / main `19fb89b926eb1a31094be999a30d5b296ff03a13`. **Scope:** review candidate PR #22, not a new published version. The presence of code is not proof of qualification; CI and live UI evidence must be recorded separately.

This tranche is deliberately **ten original catalogue entries**, not ten arbitrary buttons. “Bounded” denotes a real usable subset with stated limits. It never retires broader missing workflows or physical acceptance.

| Original catalogue line | Requirement | Change in candidate | Qualification / boundaries |
| --- | --- | --- | --- |
| 244 | **CORE** Create from templates | A chooser rooted in the user's XDG Templates folder; a selected native template is copied into the active native folder through ClipboardController's journalled conflict-safe transfer. | Sources outside the Templates prefix are refused; chooser starts in the Templates directory. No provider-based templates, templates manager or live Mint acceptance. |
| 245 | **CORE** Immediate inline rename after creation | Optional checkbox in New Folder/New File/New Sparse File dialogs; after asynchronous creation is committed, selection is reconciled and inline name editing is requested. | Best-effort for realised GTK rows with bounded retries; unavailable or navigated-away views do not become another pathname mutation. This is optional rather than the unconditional ordinary UX described in the catalogue. |
| 249 | **ADVANCED** Create common links/shortcuts from UI | Two native link commands and naming dialog reuse ItemOperationController's journal, mutation guard and worker. | POSIX link types only; no Windows shortcuts, remote links or specialised link providers. |
| 250 | **ADVANCED** Explicit sparse creation | New Sparse File dialog takes a logical MiB size; the native engine stages with `ftruncate`, flushes, atomically publishes and verifies its length. | 1 MiB–1 TiB through the GUI; core permits empty or <=1 TiB. Sparse allocation is filesystem-defined; no physical allocation guarantee on unsupported media. |
| 294 | **CORE** Create symbolic link | `symlinkat` creates a relative sibling link using no-replace namespace semantics and confirms the target text and destination-parent publication. | Native and same directory only; by design a later rename may break the link. |
| 295 | **ADVANCED** Create hard link where safe | Held `O_PATH` fd with Linux `linkat(AT_EMPTY_PATH)`, destination no-replace, exact inode verification and parent sync. | Native regular file only and same parent; unsupported filesystems/permissions fail explicitly. |
| 416 | **CORE** Recent files | File-open requests are kept separately from recent folders, displayed in the existing Recent surface and cleared alongside the folder history. | Up to 20 home-namespace native references, existing privacy exclusions and private paths policy; not OS-wide recents and no stable object identity. |
| 420 | **ADVANCED** Recently downloaded / received with origin metadata | Inspection reports available `user.xdg.origin.url` / referrer extended attributes, validated and bounded. | **Partial:** shows native origin metadata for the selected file, not an independent recently-received timeline. Absence of xattrs means unknown, not clean/not downloaded. |
| 540 | **ADVANCED** Archive preview of contents and size | Read-only libarchive header inventory, aggregate **reported** logical bytes, count, first 32 sanitised paths and unsafe-entry flags. | **Bounded:** up to 64 MiB compressed source / 5000 headers / 8 GiB claimed logical total; does not decompress-check every entry, guarantee extractability, show whole large archives, or browse them virtually. |
| 565 | **CORE** Edit ordinary permissions when authorised | Harden previously shipped chmod against user-owned mode-000 files using descriptor-bound `O_PATH` + `fchmodat2`, with safe no-follow identity and post-change durability verification. | **Existing feature hardened, not newly created.** Linux kernels lacking `fchmodat2` refuse rather than using insecure pathname fallback; ACL and privilege editing remain separate. |

## Architectural and safety rules

All new user-file mutations use the existing deterministic operation engines, conflict policy and operation journal; no GUI-owned direct unjournalled user-file changes. Relative symbolic links and hard links never dereference the selected final component; a missing capability never silently becomes a successful operation. Search, archive preview and origin inspection remain read-only. Derived recents and UI state do not become filesystem truth.

The 0.1.73 published release is immutable. This candidate does not advance VERSION, publish a release, install as the system's default manager or remove Nemo.

## Verification still required

- Exact-head CI: configure/build, all CTest executables, Debian package and aggressive native-installer qualification. Record exact SHA and run ID only after completion.
- GTK interaction acceptance for template chooser, sparse/link creation, recent-file views and immediate-rename timing.
- Permission behaviour on Linux kernels supporting and not supporting descriptor-bound chmod.
- Platform accessibility/Day/Night, real removable drives and remote providers, archive-header adversarial inputs and power-interruption qualification.

**Sign-off status:** pending evidence, draft review. No claim of full catalogue completion.
