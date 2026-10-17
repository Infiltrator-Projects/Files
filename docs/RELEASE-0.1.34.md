# Files 0.1.34

This release establishes the native InfiltratorOS installation contract for Files.

- Install the launchable application at `/Programs/files` rather than `/usr/bin/files`.
- Keep desktop integration in `/usr/share/applications` with an explicit `Exec=/Programs/files` target.
- Record build provenance in `/usr/share/doc/files/BUILD-INFO`.
- Publish a self-contained hardware-native installer alongside the regular Debian package.
- The native installer performs an aggressive `-O3`, `-march=native`, LTO build with a two-pass PGO training cycle before installing the resulting Debian package.
- Do not change the host system's default file-manager association; Files remains an additional installed application at this stage.
- CI rejects any package that recreates `/usr/bin/files` or `/System/Programs`.
