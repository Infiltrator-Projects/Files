# Files documentation

The root [README](../README.md) introduces the application. [InfiltratorOS product-family and UI contract](https://github.com/Infiltrator-Projects/InfiltratorOS/blob/main/docs/PRODUCT-FAMILY.md) is the common authority for product identity, MB Corpo typography, semantic appearance, independent installation and interface consistency.

## Maintained technical contracts

- [Engineering philosophy](PHILOSOPHY.md) — project-specific obligations under [Libraries/Common philosophy](https://github.com/Infiltrator-Projects/Libraries/blob/main/docs/PHILOSOPHY.md).
- [Architecture](ARCHITECTURE.md) — ownership, mutation, recovery and object/navigation boundaries.
- [Design](DESIGN.md) — first-principles product decisions and interaction model.
- [Features](FEATURES.md) — intended capabilities and implementation scope.
- [Common integration](COMMON-INTEGRATION.md) — shared-library mechanisms versus Files-owned semantics.
- [Product quality](PRODUCT-QUALITY.md) — task journeys and acceptance criteria.
- [Validation](VALIDATION.md) — build, test and release qualification.
- [Changelog](../CHANGELOG.md) — maintained high-level release history.

## Historical work and research

- [Codebase review (15 October 2026)](CODEBASE-REVIEW-2026-10-15.md) — dated review, not a maintained specification.
- [Research (2026)](RESEARCH-2026.md) — source investigation and design evidence.
- [UI design snapshot (0.1.39)](UI-DESIGN-0.1.39.md) — historical visual refinement record, not a replacement for Common's live design contract.
- `RELEASE-0.1.*.md` — historical release-specific records. Current product behaviour is defined by code, tests and the maintained documents above, not by superseded release notes.
- [Swarm coordinator and protocol](../swarm/COORDINATOR.md) — specialist internal coordination documentation; it is not part of the end-user product contract.

## Documentation authority

Maintain one current explanation for each concern. Do not turn release snapshots into current requirements or duplicate Common's palette, typography or spacing constants in Files docs. When modifying product behaviour, update the relevant maintained contract and its qualification evidence in the same change.
