# Files 0.1.45 — resource-only clocks

- Pin the exact qualified Common 1.19.56 commit.
- Eliminate the compatibility fallback for every extracted native clock:
  seven earlier alternatives, Chinese hundred-kè, Mean Solar, and
  explicit 12/24-hour conventional modes.
- Files retains no copy of the clock-face algorithms: date-modified values
  are formatted through Common's validated resource presentation handle.
- Missing or invalid clock resource shows a clear unavailable state rather
  than silently formatting with a duplicate compiled implementation.
- Canonical filesystem timestamps, sort keys, file-operation semantics
  and the other twelve interim clock modes remain unchanged.
