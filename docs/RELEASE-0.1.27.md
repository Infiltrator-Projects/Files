# Infiltrator File Manager 0.1.27

0.1.27 is a maintainability, durability and release-governance hardening release. It deliberately does not integrate the reviewed but unresolved session/tab candidate from PR #15.

## Engineering changes

- Operation journal records are serialized across threads/processes and written through Common's exact EINTR-safe POSIX I/O contract.
- The journal fsyncs both its record file and containing directory, repairs an incomplete crash tail before appending, and keeps files private (`0600`).
- Operation IDs carry one process-session identity; live operations from another Files window in the same application session are no longer mistaken for previous-session crash residue.
- Missing/invalid persistent state roots fail closed instead of silently moving recovery state into `/tmp`.
- Journal read/update failure is exposed in the recovery surface rather than silently appearing as an empty recovery set.
- GTK implementation sources now have one CMake target consumed by the application and hosted tests, removing duplicated source inventories.
- The exact Infiltratr Common commit is derived from the repository gitlink; CMake no longer carries a second hard-coded commit SHA, and live checkouts must still prove the checked-out submodule matches that gitlink exactly.
- Generated Python bytecode and obsolete dated swarm snapshots are removed from repository state and ignored going forward.

## Build and release changes

- Release mechanics are centralized in `ci/release.sh`; workflow YAML now orchestrates one release implementation.
- Upload/download artifact actions are pinned to immutable commits.
- A manual release-recovery input can publish the already-tested current `main` VERSION without manufacturing another version bump.
- The release authority verifies package identity/version, the SHA-256 manifest, exact release assets, immutable tag/source identity and GitHub-published asset digests.
- Central catalogue and beta APT propagation are explicitly observed after GitHub release publication.

## Qualification

The existing deterministic operation, destination ownership, transfer/recovery, mounted-place, metadata and hosted GTK tests remain part of the release gate. The operation-journal fixture additionally qualifies live-session exclusion, legacy interrupted-operation recovery, damaged-tail repair, private permissions and fail-closed missing-state behaviour.
