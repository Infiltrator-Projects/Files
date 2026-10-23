# Validation

## Qualification

From a recursive checkout with GTK 4, GIO, CMake and Ninja installed:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Install Xvfb and dbus-run-session to include the headless GTK window-state test. A configure without those tools reports that limitation; CI installs them and runs the full suite.

Core fixtures exercise location and mounted-place identity, operation outcomes, destination ownership, transfer/batch semantics, recovery and durable journalling. UI-linked fixtures exercise date-provider metadata and window/controller lifetime. The architecture defines the ownership and failure contracts these fixtures protect.

### Safety regressions

`transfer-safety` links the production core with test-only linker wrappers for fsync and fsetxattr. It requires source removal to follow copied-file and destination-parent synchronization; injects data/publication/metadata failures; verifies user attributes (including empty values), named-user ACLs and absence of inherited default ACLs; checks cancellation during durability; and qualifies malformed-journal batch refusal and exact empty-file failure residue. The test needs ordinary Linux user-xattr/POSIX-ACL support in its disposable temporary directory, as supplied by hosted qualification.

`operation-lifetime` exercises actual New Folder, New Empty File and Rename controllers while destroying their window before completion, and refuses a pending structured clipboard Paste after its window leaves the application. Live and late conflict continuations cover Skip, Keep Both and Replace in both Copy and Cut modes; late responses must leave both source files and the existing destination unchanged. It qualifies overlapping/revoked monitor leases and an actual GApplication main loop with delayed worker fsync, proving that closing the last window waits for durable completion. Run it under the same fatal-critical policy as the other headless GTK fixtures. These targeted tests protect failure contracts; they do not substitute for physical unplug/power-loss, remote-provider or accessibility acceptance.

`workspace-features` covers asynchronous path/file opening, relative paths, tab movement and stale drag rejection, legacy bookmark import, label persistence, reorder propagation across windows, and normal-close session restoration. Malformed or oversized sessions must leave the current workspace intact. `batch-removal-engine` qualifies whole-selection preflight, duplicate rejection, early/between-item cancellation, recursive removal without following symlinks, platform Trash, per-item journal completion and corrupt-journal refusal. `operation-lifetime` additionally exercises clipboard/drop progress cancellation and actual multi-item delete confirmation, cancellation, revoked late replies and batch Trash.

The 0.1.60 GTK interaction test qualifies pattern matching, matching-type selection, multi-folder tab opening and complete native-path/URI clipboard export against an enumerated temporary folder. It requires these actions to remain independent of the deterministic mutation engine. The 0.1.57 local build passed all 17 application test executables. The 0.1.59 follow-up qualifies the Shift-only in-process Move drag contract under the same full hosted release suite. The 0.1.58 tranche adds workspace regressions for saved per-folder views, refresh selection/history retention and bookmarked new-tab/new-window navigation; hosted qualification for this release must still pass the same full test suite. Eleven ran through CTest; the six GUI executables ran with fatal GTK criticals on a TCP Xvfb/session-bus fixture because this environment prohibits the Unix sockets used by the standard CTest launcher. Hosted qualification still uses the unmodified CTest commands. This is functional/regression evidence; no new screenshot-based polish or physical-device acceptance is claimed.

## Dependency and package identity

VERSION owns product identity. The Common gitlink owns its exact dependency commit. CMake's minimum Common API version expresses capability requirements independently of which compatible revision is selected.

CI builds the product once, runs CTest, packages the same build, validates Debian package identity and dependencies, and inspects its executable and desktop entry. Debian and aggressive native installer qualification require `Exec=/Programs/files %U` and `MimeType=inode/directory;` in the package and installed desktop entry, matching the executable's `G_APPLICATION_HANDLES_OPEN` contract. SHA-256 sums accompany the preserved package.

## Publication and maintenance

An unpublished VERSION retains release intent through corrective commits. Publication rechecks current main and consumes only the qualifying artifact. Drafts may be repaired; remote bytes are compared before and after publication. A public release with missing or different assets fails verification without modification.

The central repository owns APT publication. CI must run `ci/release.sh wait-apt dist` after the immutable GitHub release and must not be green until the central catalogue and beta amd64 Packages both show the exact Files version. Publishing outages retain the valid GitHub release but fail the publication observation.

Merged-branch cleanup requires the current branch SHA to match a merged PR head, no open PR, and an exact-head deletion lease. Reusing an old branch name for new work cannot authorize its deletion.

## Extended typography packaging

Standalone release qualification must inspect the Debian control archive for an executable `postinst`, a package-installed `/usr/lib/files/ensure-extended-fonts` helper from pinned Common and dependencies sufficient for verified font acquisition (`ca-certificates`, `fontconfig`, and `curl | wget`). It must not contain private duplicated MB/Daimler TTF payloads. A live standalone installation uses the same resource cache as an InfiltratorOS installation, without changing default file-manager association or existing Files data operations.
