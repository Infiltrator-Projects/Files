# Files 0.1.44 — safety and publication hardening

This patch follows Files 0.1.43.

- Permanent deletion quarantines the selected filesystem identity before recursive removal; failure reports possible partial changes and retained evidence.
- Cross-volume move verification compares actual bytes, with a same-length source rewrite regression.
- Journal scanning recognises owners still running in another Files process, reports malformed complete records unhealthy and fails closed above 128 MiB.
- CI requires the exact published GitHub version to appear in the public central catalogue and beta amd64 APT index.
- The swarm protocol and roadmap are aligned to the actual comment-backed coordinator and 0.1.43 baseline.

Limits: Large journals require manual inspection/archival until automatic rotation is added. PID reuse can conservatively defer recovery. Byte comparisons detect common races but do not create a stable filesystem snapshot of an actively modified file. Release success requires exact-head CI and observed public publication.
