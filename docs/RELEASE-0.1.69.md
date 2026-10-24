# Files 0.1.69 — Five bounded improvements

This release advances the first five priority areas of the original Files catalogue, without claiming that all their broader acceptance requirements are finished.

## Source scope

1. **Dual pane:** Second-pane A–Z/Z–A name sorting wraps directory enumeration without changing authoritative item names. Cross-pane drag/drop resolves the original directory behind the sort model.
2. **Archives:** Create ZIP archive packs one selected native regular file, at most 1 GiB, into a collision-free sibling ZIP. The operation is journalled, staged privately, output-flushed and published through the existing durable transfer engine. The source remains untouched. Directory ZIP creation, encrypted archives and archive browsing remain unfinished.
3. **Permissions:** Native owner/group/other mode editing has synchronised octal and checkbox controls. Special permission bits cannot be inadvertently stripped through the checkboxes. ACL and ownership editing and scoped privilege elevation remain future work.
4. **Remote:** Copy-only GIO imports now refuse files larger than 1 GiB both from reported size and live progress; cancellation and private-staging failure evidence retain the existing contract. Remote write, move, rename and recursive provider imports are not implemented.
5. **MTP/PTP:** GIO-provided MTP and gphoto2 copy-only file streams reuse the same import engine; provider disconnect and disappeared-item failures now have distinct messages. Physical device and hardware-removal qualification remains outstanding.

## Required acceptance

The exact versioned commit must pass GitHub Verify build, the 22-test CTest suite, Debian package and aggressive native-installer qualification. Publication requires matching immutable GitHub assets/tag and central beta APT SHA-256. A previous commit's green CI is not evidence for a new commit.

Outstanding acceptance includes full dual-pane parity, all archive formats/workflows, ACL/ownership mutation, general remote operations, real-device MTP/PTP import/disconnection matrices, physical failure injection and Mint Day/Night accessibility review. The original docs/FEATURES.md remains the governing catalogue and has not been narrowed.
