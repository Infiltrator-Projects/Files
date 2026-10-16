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

## Dependency and package identity

VERSION owns product identity. The Common gitlink owns its exact dependency commit. CMake's minimum Common API version expresses capability requirements independently of which compatible revision is selected.

CI builds the product once, runs CTest, packages the same build, validates Debian package identity and dependencies, and inspects its executable and desktop entry. SHA-256 sums accompany the preserved package.

## Publication and maintenance

An unpublished VERSION retains release intent through corrective commits. Publication rechecks current main and consumes only the qualifying artifact. Drafts may be repaired; remote bytes are compared before and after publication. A public release with missing or different assets fails verification without modification.

The central repository owns APT publication. Its scheduling and availability do not invalidate a qualified GitHub release. The explicit `ci/release.sh wait-apt dist` command remains available for publication observation.

Merged-branch cleanup requires the current branch SHA to match a merged PR head, no open PR, and an exact-head deletion lease. Reusing an old branch name for new work cannot authorize its deletion.
