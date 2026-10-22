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

`operation-lifetime` exercises actual New Folder, New Empty File and Rename controllers while destroying their window before completion, and refuses a pending structured clipboard Paste after its window leaves the application. It qualifies overlapping monitor leases and an actual GApplication main loop with delayed worker fsync, proving that closing the last window waits for durable completion. Run it under the same fatal-critical policy as the other headless GTK fixtures. These targeted tests protect failure contracts; they do not substitute for physical unplug/power-loss, remote-provider or accessibility acceptance.

## Dependency and package identity

VERSION owns product identity. The Common gitlink owns its exact dependency commit. CMake's minimum Common API version expresses capability requirements independently of which compatible revision is selected.

CI builds the product once, runs CTest, packages the same build, validates Debian package identity and dependencies, and inspects its executable and desktop entry. SHA-256 sums accompany the preserved package.

## Publication and maintenance

An unpublished VERSION retains release intent through corrective commits. Publication rechecks current main and consumes only the qualifying artifact. Drafts may be repaired; remote bytes are compared before and after publication. A public release with missing or different assets fails verification without modification.

The central repository owns APT publication. CI must run `ci/release.sh wait-apt dist` after the immutable GitHub release and must not be green until the central catalogue and beta amd64 Packages both show the exact Files version. Publishing outages retain the valid GitHub release but fail the publication observation.

Merged-branch cleanup requires the current branch SHA to match a merged PR head, no open PR, and an exact-head deletion lease. Reusing an old branch name for new work cannot authorize its deletion.

## Extended typography packaging

Standalone release qualification must inspect the Debian control archive for an executable `postinst`, a package-installed `/usr/lib/files/ensure-extended-fonts` helper from pinned Common and dependencies sufficient for verified font acquisition (`ca-certificates`, `fontconfig`, and `curl | wget`). It must not contain private duplicated MB/Daimler TTF payloads. A live standalone installation uses the same resource cache as an InfiltratorOS installation, without changing default file-manager association or existing Files data operations.
