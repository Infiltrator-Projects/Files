# Infiltrator File Manager 0.1.29

Version 0.1.29 is the qualified result of the 15 October 2026 whole-codebase brittleness and maintainability review. The earlier 0.1.28 candidate was rejected by CI before publication after an operation-engine regression test exposed a staging-cleanup edge case; the correction and the remaining review changes were requalified before this release.

## Structural hardening

- **New Folder uses the destination-ownership boundary.** A new directory is assembled under descriptor-rooted private staging, its object identity is recorded, and the directory is published with atomic no-replace semantics before identity verification. An ordinary existing-name check remains as an early cleanup-avoidance observation; the atomic publication is still the final namespace authority.
- **Transfer aggregate arithmetic has one checked authority.** Byte and item totals now reject `uintmax_t` overflow instead of allowing progress or verification counts to wrap.
- **Redundant transfer scans were removed.** Copy no longer walks an entire tree only to calculate progress when no progress controller exists or when a batch controller already owns the aggregate totals.
- **Theme state now uses Common's durable POSIX state primitives.** Appearance persistence uses the pinned Common XDG path, bounded read, recursive directory creation and durable atomic private-file publication rather than a second GLib persistence path.

## Build and release hardening

- The Debian package architecture is validated explicitly as `amd64`, matching the central Infiltrator Repository contract.
- The release authority has one architecture constant for package identity, asset naming and APT observation.
- The workflow promotes the exact package that passed the build/test/package job instead of rebuilding a release artifact later.
- Tag identity, release asset set and GitHub asset SHA-256 digests are verified against the exact tested artifact.
- Public catalogue and beta APT propagation are a required release result rather than a non-blocking informational check.

## Review conclusion

The descriptor-rooted destination ownership, cross-volume recovery/quarantine, durable journal and event-driven swarm remain deliberately sophisticated because their complexity protects user data or deterministic coordination. They were not flattened merely to reduce line count. The review removed accidental complexity where a second authority, weaker contract, unchecked arithmetic or redundant work existed without buying an equivalent guarantee.
