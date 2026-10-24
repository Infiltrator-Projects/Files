# Files 0.1.67 — release source and qualification

This release brings the 24 post-v0.1.66 source commits into a **new immutable version**, rather than retargeting v0.1.66. The maintained implementation ledger and still-open original requirements are in [FEATURE-AUDIT-0.1.67.md](FEATURE-AUDIT-0.1.67.md).

The \`VERSION\` change is release intent. \`.github/workflows/ci.yml\` builds, tests and packages the exact new main commit. Publication is only successful when its **publish** job verifies GitHub tag/assets and \`ci/release.sh wait-apt\` verifies central catalogue and beta APT Debian checksum. Hosted Verify green without publish is not equivalent to a distributed release.

This file records source scope; it does **not** pre-announce successful publication. The release page, artifact identities, APT catalogue and exact pipeline must be checked after the workflow completes. Full live Linux Mint Day/Night, accessibility, hardware/failure and broad provider/IFS qualifications remain separate acceptance obligations.
