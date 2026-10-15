# Infiltrator File Manager 0.1.30

Version 0.1.30 is the final qualified result of the 15 October 2026 whole-codebase brittleness and maintainability review.

## Core hardening

- **New Folder now uses the destination-ownership boundary.** A directory is assembled in descriptor-rooted private staging, its identity is recorded, and it is published with atomic no-replace semantics before identity verification. An early existing-name observation avoids needless staging but is not trusted as the final namespace authority.
- **Transfer aggregate arithmetic has one checked authority.** Batch byte/item totals and transfer verification counts reject `uintmax_t` overflow rather than silently wrapping.
- **Redundant tree scans were removed.** Copy no longer walks an entire source tree merely to calculate totals when no progress controller is observing them or when a batch controller already owns the totals.
- **Theme preference state now uses Common's durable POSIX primitives.** XDG path resolution, bounded reads, directory creation and atomic private-file publication no longer have a second GLib persistence path.

## Build and release hardening

- The build/test/package job is the sole producer of the release package; publication consumes that exact artifact rather than rebuilding it.
- Debian package name, version, architecture, GTK dependency, executable and desktop entry are validated before checksums are created.
- The release authority verifies the tested commit, release tag, exact asset set and GitHub-reported SHA-256 asset digests.
- Package architecture is explicitly `amd64`, matching the current central repository contract.
- Public catalogue and beta APT visibility are required release evidence rather than a non-blocking informational check.
- The public-publication observation window is fifteen minutes, materially longer than the central repository's five-minute recovery schedule, while remaining bounded and fail-closed.
- A newer commit on the same ref now cancels an older verification run, preventing a superseded release-intent candidate from racing a corrective commit to publication.

## Qualification history

The first 0.1.28 candidate was rejected by the operation-engine regression test before publication. The defect was corrected and all ten regression executables plus Debian package validation passed.

Version 0.1.29 then proved the GitHub release path and exact-artifact verification, but its workflow correctly remained red because public APT/catalogue propagation was not observed inside the original five-minute window. That live failure exposed a timing contract that was too tight relative to the central repository's own five-minute recovery publisher. Version 0.1.30 contains the corrected bounded observation window and stale-run cancellation rule and is the qualified release from that corrected machinery.

## Deliberate complexity retained

Descriptor-rooted ownership, cross-volume quarantine/recovery, the durable operation journal and the event-driven swarm remain intentionally sophisticated. Their complexity corresponds to real data-safety, interruption-recovery or coordination boundaries and was not removed simply to reduce line count.
