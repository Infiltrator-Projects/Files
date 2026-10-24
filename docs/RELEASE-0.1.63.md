# Files 0.1.63 — Qualified recursive search tranche

## Ten delivered capabilities

1. Recursive filename search beneath the current GIO directory, with an explicit option to restrict the search to direct children.
2. Exact filename matching.
3. Case-sensitive or Unicode case-folded matching.
4. Filename wildcard matching with `*` and `?`.
5. Restrict results to regular files, directories or both.
6. Minimum logical file-size filtering in exact bytes.
7. Maximum logical file-size filtering in exact bytes.
8. Modified-on-or-after date filtering using the user's local civil date.
9. Bounded case-sensitive/insensitive UTF-8 substring search in recognized text files up to 1 MiB.
10. Reveal a selected search result in its containing folder, selecting the original item and preserving navigation through the existing window model.

Results expose the provider's URI and display path without adding a separate index or mutating filesystem objects. Advanced search is launched from File actions > Search in subfolders…; Ctrl+F retains the faster current-folder list filter. Additional options include hidden-item inclusion and cancellable background enumeration.

## Correctness and limits

- All work runs off the GTK main thread through GTask, using a captured GFile root and GCancellable. A closed search window cancels its worker; weak GTK references prevent workers from accessing destroyed dialogs.
- Traversal never follows symbolic links and refuses to cross a native mount boundary when the provider supplies comparable device identity.
- Search stops after 100,000 visited entries, 500 results or 64 nested levels. Every stop, inaccessible subdirectory and provider failure is explicitly labelled incomplete; partial hits are never represented as an exhaustive search.
- Content examination is limited to recognized text formats, UTF-8, and files no larger than 1 MiB; files that cannot be examined are counted and identified as unsearched. No arbitrary binary/archive/PDF parsing, system-wide indexing or semantic search is claimed.
- Filename matching and content searching use separate, validated criteria. Missing file size/date metadata does not masquerade as zero or the Unix epoch.
- The result's GFile URI is passed to the existing FileManagerWindow navigation API for exact parent-folder reveal. Search never acquires mutation authority or bypasses any journalled operation controller.

## Qualification

The hosted Ubuntu 24.04 source build completed successfully with **18/18 CTest tests passed**, including the new GIO search-engine fixture and the real GTK search-dialog/reveal interaction. The qualified native installer and Debian package paths completed successfully before the 0.1.63 release-intent commit.

Physical-device, large-tree stress, multi-provider and visual acceptance are not proven by these hosted tests. The app remains in its deliberately staged feature-completion programme.
