# Infiltrator File Manager 0.1.28

This release is the result of a whole-codebase brittleness and maintainability review following 0.1.27.

## Structural hardening

- New Folder now uses the descriptor-rooted destination-ownership boundary: it is assembled privately, published with atomic no-replace semantics, and verified by recorded object identity rather than relying on a path precheck followed by path-only verification.
- Batch transfer measurement now rejects byte or item-count overflow explicitly instead of allowing aggregate progress values to wrap.
- Theme preference persistence now uses the pinned Common XDG path and durable atomic-file primitives rather than a separate GLib state-publication path.

## Release hardening

- Debian architecture is explicitly validated by both the build gate and release authority.
- The release authority has one local architecture constant for asset naming and APT observation.
- Central catalogue and beta APT observation is now a required release job result rather than a non-blocking informational check.

## Review conclusion

The destination-ownership/recovery core and event-driven swarm remain deliberately complex because their complexity protects user data or deterministic coordination. They were not flattened merely to reduce line count. The review concentrated changes where duplicate authority, weaker semantics or silent failure could be removed without weakening those guarantees.
