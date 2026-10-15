# File Manager whole-codebase review — 15 October 2026

## Scope

This review covered the repository as an engineering system rather than only the release workflow: governing philosophy and design records, architecture and module boundaries, core and UI source, ownership/lifetime semantics, filesystem mutation and recovery, Common integration, configuration/state persistence, CMake and dependency pinning, regression tests, GitHub Actions, Debian packaging, release publication, swarm coordination and repository hygiene.

## Architecture assessment

The current decomposition is sound for the product's maturity. `FileManagerWindow` owns browse/navigation composition; dedicated controllers own creation, single-item transfer, batch transfer, destructive/recovery operations, journal inspection and theme interaction. Core operation policy is separated from GTK. `Location` and mounted-place modelling preserve the provider boundary rather than allowing UI paths to become filesystem policy.

The largest core mechanisms — destination ownership, recovery/quarantine and the durable operation journal — are intentionally more complex than ordinary file-manager helper code. That complexity is justified by explicit no-overwrite, object-identity, rollback and interruption guarantees. Removing those layers would reduce line count by weakening correctness, which conflicts with the governing proportional-complexity rule.

The swarm coordinator is likewise intentionally event-driven with a scheduled recovery pass. Its state machine and role separation are coordination policy, not release machinery duplicated in the product build.

## Brittleness and duplication corrected

### New Folder ownership

`OperationEngine::create_directory()` previously used a weaker pathname-oriented mutation and verification path than copy/move. It now uses the same descriptor-rooted destination-ownership boundary: private staging, recorded object identity, atomic no-replace publication and identity verification. A preflight existing-name observation is retained only to avoid needless private staging; it is not trusted as the final race boundary.

### Aggregate arithmetic

Batch and transfer accounting previously had unchecked addition paths. One local `uintmax_t` checked-add authority now protects byte/item totals and verification entry counts. Pinned Common 1.19.41 does not expose a checked unsigned-add primitive covering this type, so the local seam is explicit rather than pretending a weaker shared helper satisfies the contract.

### Redundant transfer traversal

Single-item copy formerly scanned the complete source tree for totals even when progress was not observed, and batch copy could repeat a full traversal after the batch had already measured every item. Copy now scans only when a non-batch progress controller actually needs totals. The data-copy traversal and post-copy verification remain because they serve different correctness guarantees.

### Theme state persistence

Appearance preference persistence formerly used GLib file publication while the project already requires Common's durable POSIX state machinery for Files-owned state. It now resolves the XDG configuration root and reads/writes the preference through pinned Common, including durable atomic private-file publication.

### Release authority

The build/release path now has a clearer single chain of custody: one build/test/package job produces and validates the Debian package; a release job receives that exact artifact; `ci/release.sh` verifies commit identity, package identity, tag identity, exact release assets and their SHA-256 digests; public catalogue and beta APT visibility must then be observed successfully before the workflow is green.

The package architecture is explicitly checked as `amd64`, matching the central repository's current File Manager publication contract, and the release script uses one architecture constant instead of repeating the value in independent release checks.

## Build and dependency assessment

The build uses C++20, GTK 4/GIO and an exact Common gitlink. CMake rejects a floating or mismatched Common checkout in a normal Git build and links the qualified `InfiltratrCommon::Common` target rather than copying Common implementation sources into File Manager.

Compiler warnings are deliberately enabled at a useful level. GTK 4.14 reports the existing `gtk_css_provider_load_from_data()` compatibility call as deprecated; it is retained while the project still advertises GTK 4.10 compatibility rather than silently raising the platform floor as a warning cleanup. The remaining warning-level observations are not release correctness failures and do not alter the ownership or publication contracts reviewed here.

## Test assessment

The hosted gate covers ten regression executables spanning location modelling, detail metadata, mounted places, GTK window state/lifetime, ordinary operations, destination ownership, recovery, transfer, batch transfer and the operation journal. The GTK regression executes headlessly under Xvfb/DBus in CI. Packaging is built only after the tests pass and is then inspected for package name, version, architecture, GTK dependency, executable and desktop entry before checksums are generated.

The review itself exercised the gate as intended: the first 0.1.28 candidate built but failed `operation-engine`; publication was therefore skipped. The failing edge case was corrected, and subsequent commits passed all ten tests plus Debian package validation before the final release version was cut. This is evidence that the gate is preventing publication rather than merely decorating it.

## Repository and release hygiene

The intended steady state is one branch, `main`; no open pull requests or temporary review branches; an exact release tag on the qualified release commit; exactly the expected Debian package and checksum assets; and the same version visible in the central catalogue and beta APT repository. The release workflow now treats the last two publication observations as required evidence.

## Deliberate complexity retained

The following were reviewed and retained rather than simplified away:

- descriptor-rooted protected staging and object-identity verification;
- cross-volume move quarantine/restore sequencing;
- durable START/END operation journalling and interrupted-operation inspection;
- separate ordinary, transfer, batch and destructive/recovery operation layers;
- mounted-place identity qualification and provider-aware browsing boundaries;
- the event-driven swarm coordinator with scheduled recovery fallback.

Each exists because it owns a materially different failure or concurrency boundary. Combining them would make changes shorter locally but more coupled globally.

## Result

After the changes above, the principal maintenance risks found by this review no longer depend on competing authorities or silent assumptions. Core filesystem safety remains fail-closed, progress accounting is overflow-aware, unnecessary traversal has been reduced, Files-owned theme state uses the shared durable substrate, and release success now means the tested package has actually propagated through the public Infiltrator publication path.
