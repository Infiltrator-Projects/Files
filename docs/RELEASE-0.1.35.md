# Files 0.1.35

This release fixes hardware-native installation on real user machines.

- Keep the complete Files test suite as a release-CI gate before publication.
- Stop using host-dependent `mounted-places` and graphical `window-state` tests as end-user installation gates during the PGO generation pass.
- Use the deterministic test subset as the native PGO workload so an otherwise valid installation cannot fail because the target machine has a different live mount or desktop-session topology than the GitHub release runner.
- Retain the aggressive `-O3`, `-march=native`, LTO and two-pass PGO build contract.
- Retain `/Programs/files` as the canonical executable and do not change the host's default file manager.

The change addresses InfiltratorOS provisioning failures where the native installer returned CTest status 8 during the PGO training phase on a real workstation.
