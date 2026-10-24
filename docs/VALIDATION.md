# Validation

## 0.1.68 native package identity and upgrade regression

The InfiltratorOS field log for Files 0.1.67 reached 66/66 C/C++ compilation steps and generated its native DEB, then APT refused the final native package transaction as a downgrade. The native installer's source-version-equivalent package identity was ambiguous with the separately published generic APT build.

The production native installer now retains the product source release but assigns a `+native1` Debian package suffix *only* to the final trained, aggressive PGO package. The ordinary release package remains at the unchanged source version. The system-mode installer fails before the expensive two-pass build if a genuinely newer Files package is installed. Release qualification installs the generic package first, runs the actual native compiler and verifies that APT upgrades it to the exact native-versioned package without `--allow-downgrades`. The ordinary runtime, GTK toolkit, package payload and file-operation engines are otherwise unchanged. This source change is not itself proof of publication or machine-level installation.



## 0.1.67 source/release evidence

The [0.1.67 implementation audit](FEATURE-AUDIT-0.1.67.md) anchors the 24 post-v0.1.66 changes to source, original requirements and tests. The prior main source `d562d113...` passed 20 hosted CTest executables and package/native-installer checks in [Verify run 40075635698](https://github.com/Infiltrator-Projects/Files/actions/runs/40075635698). This is evidence for the **earlier exact commit only**. The 0.1.67 versioned/audited commit requires **its own** successful Verify, GitHub asset/tag qualification and central beta APT SHA-256 match. Physical Linux Mint Day/Night, accessibility, cross-filesystem and unplug/crash tests remain explicitly unproven.


## Qualification

The 0.1.66 extension/identity/created-time tranche adds explicit `detail_metadata_test` contract checks for presence-aware raw creation sorting, missing timestamp behaviour, extension-preserving underlying names, symlink identity, hidden identity and special file type. `interaction_surface_test` drives real GTK action surfaces to toggle filename extensions, check updated command labels and ensure selection remains identical. Tested source passed 20/20 hosted CTest suites and native-installer/DEB qualification. Release, APT digest and physical Linux Mint visual acceptance are independently tracked.


The 0.1.65 tranche adds `tag-catalogue` and `search-persistence` CTest executables (raising the hosted suite to 20), including malformed durable-state refusal and exact read-back checks. Search engine fixtures exercise duplicate SHA-256 grouping, UID filtering, modified intervals, per-query exclusions, tag-URI matching and symlink-cycle avoidance. Real GTK tests cover selected-root aggregation, named-search save/reload and expected checksum verification/mismatch including basename-bound sha256sum lines. Exact revision CI, native-install qualification and beta APT publication remain release gates; no headless test proves desktop aesthetic acceptance.


Files 0.1.64 interaction-surface qualification exercises Quick Look on a real temporary directory containing plain text, PNG and intentionally unsupported binary content. It asserts clipboard text/path fidelity, image decode/zoom availability, neighbour actions and explicit binary fallback. The preview work captures GFile references and uses a cancellable generation-fenced background GTask, refusing unknown or >8 MiB inputs. The hosted Ubuntu 24.04 build passed 18/18 CTest executables and the aggressive native-installer qualification. Actual Mint Day/Night screenshots, accessibility review and remote-provider stress remain unproven.


The 0.1.63 GIO search-engine test exercises recursive versus direct-child search, exact/Unicode-folded and case-sensitive wildcard matching, file/folder filters, minimum/maximum byte limits, modified-since filtering, content matching, symlink-cycle refusal and cancellation. The actual GTK interaction test opens Search files, finds a nested file and requires Reveal in containing folder to navigate to its parent and select the exact original item. Hosted qualification on Ubuntu 24.04 passes 18 tests and the native-installer/package gate prior to release intent; no stress-performance or GUI screenshot claim is inferred.


Files 0.1.62's interaction-surface regression additionally opens real GTK inspection windows, waits for asynchronous GIO metadata, computes SHA-256, compares two dissimilar test files, exports the visible report through the clipboard, and verifies an empty-directory recursive scan with allocated/logical result fields. A native folder exposes terminal launch without starting a terminal in CI. The inspection worker has a cancellation contract and explicit 250,000-entry/64-level traversal limits; device and visual qualification remain separate.


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

CI builds the product once, runs CTest, packages the same build, validates Debian package identity and dependencies, and inspects its executable and desktop entry. Release jobs run on pinned `ubuntu-24.04` GitHub-hosted runners because package preparation and aggressive installer qualification need unattended privileged APT and system installation. A self-hosted Linux runner cannot replace that environment until its admin config permits noninteractive `sudo` and the complete job qualifies without weakened assertions; `sudo` password prompts must never be worked around by embedding secrets in workflow scripts. Debian and aggressive native installer qualification require `Exec=/Programs/files %U` and `MimeType=inode/directory;` in the package and installed desktop entry, matching the executable's `G_APPLICATION_HANDLES_OPEN` contract. SHA-256 sums accompany the preserved package.

## Publication and maintenance

An unpublished VERSION retains release intent through corrective commits. Publication rechecks current main and consumes only the qualifying artifact. Drafts may be repaired; remote bytes are compared before and after publication. A public release with missing or different assets fails verification without modification.

The central repository owns APT publication. CI must run `ci/release.sh wait-apt dist` after the immutable GitHub release and must not be green until the central catalogue and beta amd64 Packages both show the exact Files version. Publishing outages retain the valid GitHub release but fail the publication observation.

Merged-branch cleanup requires the current branch SHA to match a merged PR head, no open PR, and an exact-head deletion lease. Reusing an old branch name for new work cannot authorize its deletion.

## Extended typography packaging

Standalone release qualification must inspect the Debian control archive for an executable `postinst`, a package-installed `/usr/lib/files/ensure-extended-fonts` helper from pinned Common and dependencies sufficient for verified font acquisition (`ca-certificates`, `fontconfig`, and `curl | wget`). It must not contain private duplicated MB/Daimler TTF payloads. A live standalone installation uses the same resource cache as an InfiltratorOS installation, without changing default file-manager association or existing Files data operations.
