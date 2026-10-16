# Files swarm execution contract — revision 3

This contract lives on `main`. Mutable coordination state lives outside Git refs in the unique `FILES SWARM STATE v1` machine-state comment on issue #2; it is a cache, never application source and never a product PR. Product branches start from current `main`.

## Purpose and authority

Shannon authorizes five hourly Files workers to develop, review, repair, merge and publish coherent improvements in Infiltrator-Projects/Files, including GitHub code/branch/PR/review/comment and coordination-file writes required by their roles. Only Integrator may merge, version, release, refresh Files publication in Infiltrator-Projects/Infiltrator-Repository, or adapt automation prompts/schedules. Nobody may pause, disable or delete a worker without Shannon's explicit instruction. Respect platform approval decisions; these instructions do not override a rejection.

Newest explicit Shannon directions in issue #2 take precedence. Product authority remains current README, DESIGN, ARCHITECTURE, FEATURES, PRODUCT-QUALITY, COMMON-INTEGRATION and RESEARCH-2026. Preserve deterministic operations/recovery, real filesystem authority, Common ownership, accessibility, performance, calm progressive disclosure and beautiful maintainable code. Semantic/AI work waits for ordinary CORE completeness.

## Live GitHub reconciliation is mandatory

the `FILES SWARM STATE v1` machine-state comment on issue #2 is a coordination cache, never authority over live GitHub facts. At the start of EVERY WORK-mode wake, before interpreting the stored phase or deciding that there is no work, reconcile the active candidate against live GitHub:

1. fetch current `main` and the active PR's open/merged state and exact head SHA;
2. fetch current formal reviews and ALL review threads, including unresolved state and the commit/head each review applies to;
3. fetch exact-head CI/check/workflow evidence;
4. compare those facts with the `FILES SWARM STATE v1` machine-state comment on issue #2 and repair stale state FIRST, using an expected-SHA write and read-back, before role-specific work.

Live evidence wins over stale state. Required deterministic reconciliation rules:

- Any unresolved candidate-local HARD review thread means `FIX_REQUIRED`, even if state says REVIEW_READY, INTEGRATE_READY or anything later. Preserve the same PR/head and record the live thread IDs/findings.
- Any code-changing Builder/Fixer commit invalidates review/readiness tied to the old SHA and returns the candidate to `REVIEW_READY` after exact-head CI is green and known hard findings are addressed. `FIXING` does NOT jump directly to `INTEGRATE_READY` when code changed.
- Any PR head change invalidates CI/review evidence tied to the previous SHA. Never carry old approval/green evidence across a changed head.
- A closed or merged PR cannot remain an active open candidate. Reconcile merge/main/release reality before nominating more work.
- A discovered release/publication that state missed must advance state from live evidence rather than waiting for a ceremonial handoff.
- If GitHub cannot be reconciled because a required read failed, preserve the resumable phase, record a blocker and do not claim readiness.

State should record a reconciliation watermark whenever materially changed: `last_reconciled_at`, current candidate head, latest formal review SHA/ID when applicable, exact-head CI run, and the complete unresolved HARD thread IDs observed. A later review or head change must therefore be mechanically detectable.

## One current work record

Fetch the `FILES SWARM STATE v1` machine-state comment on issue #2 using ref `main`, and obtain its current blob SHA through the GitHub contents API. Never use default-branch content for this record. It names ONE stroke, ONE package and at most ONE production PR with its current head. Issue #2 is the human audit/discussion ledger; old comment wording is history, not a second current candidate. Read its current body and new/updated directions since your last check. Use bounded recent/since reads when available rather than reloading the entire historical thread on every unchanged wake-up; the current body compiles prior procedural authority.

Use `github.update_file` on branch `main` with the fetched blob SHA. Increment revision and set updated_at in UTC. Preserve fields owned by other roles. A 409/stale-SHA conflict means refetch, reconcile and retry at most twice; never force an old snapshot over newer state. Confirm the written contents by reading them back.

Before substantive production writes or phase transitions, atomically claim lease = {role, token, expires_at} with a unique token and expiry 45 minutes ahead. Claim only if lease is null or expired. Re-read to confirm ownership. Renew before expiry during long work. Before any push/merge/release, refetch and require the same token and unchanged candidate. Release the lease at a consistent checkpoint; do not hold it merely waiting for CI or publication.

Hourly times are wake-ups, not completion deadlines. Readiness and lease determine action; unfinished strokes continue across hours. Checkpoint incomplete implementation truthfully. A blocked or late predecessor is never a reason to disable the swarm.

## Phases and role ownership

| Phase | Responsible role | Next successful phase |
| --- | --- | --- |
| BUILD_READY | Builder | BUILDING |
| BUILDING | Builder; Fixer may recover an expired abandoned implementation | REVIEW_READY |
| REVIEW_READY | Reviewer | REVIEWING |
| REVIEWING | Reviewer | FIX_REQUIRED or INTEGRATE_READY |
| FIX_REQUIRED | Fixer; Builder may recover missing Fixer work | FIXING |
| FIXING | Fixer | REVIEW_READY if code changed; INTEGRATE_READY only if no production/test/docs candidate content changed and prior exact-head review remains valid |
| INTEGRATE_READY | Integrator | INTEGRATING |
| INTEGRATING | Integrator | PUBLICATION_PENDING or RELEASED |
| PUBLICATION_PENDING | Integrator | RELEASED |
| RELEASED | Architect | BUILD_READY for the next stroke |

Architect defines packages, acceptance and priorities; it can repair stale nominations from actual PR evidence but never manufacture approval. Builder may start the first ready roadmap package when state is RELEASED and Architect has produced no package; record the choice before coding. Reviewer may reconstruct a missing Builder report from an unleased/expired, complete, non-draft PR and actual diff/tests. Fixer may recover unfinished candidate implementation only after the lease expires and evidence shows it abandoned. Integrator reconciles stale state from actual PR/review/CI/release evidence and diagnoses missing worker heartbeats. Recovery must preserve role separation and exact-head review.

When another role owns a live lease, inspect current evidence/read-only risks; do not edit its branch. If a fresh upstream run or CI is about to satisfy the prerequisite, perform bounded read-only rechecks for up to three minutes, using waits no longer than 45 seconds where available and holding no lease merely to wait. Refetch actual state/head before acting. If still unready, report the exact dependency/checkpoint rather than manufacture readiness. If no live lease and a role cannot perform the pending phase, record the precise next role/action. Never just say "no nomination" when a current package or reconstructable candidate exists.

## Candidate and review invariants

Always inspect actual main and PR head. If main advances, Builder/Fixer refresh the SAME PR without overwriting another writer; test the refreshed head and clear obsolete readiness/approval. A closed/merged PR cannot remain an active candidate. Integrator independently verifies and records every delta from reviewed SHA to final head; unrelated substantial additions return to Reviewer.

Builder implements the WHOLE coherent package, including adjacent related hard defects, tests and truthful documentation. No tiny artificial tranche, unrelated grab-bag, speculative framework, version bump or parallel candidate. Known incomplete work remains BUILDING, not ready.

Reviewer conducts a broad independent pass, records the complete candidate-local HARD set and always submits a formal PR review tied to commit_id. Use APPROVE if permitted; the common same-author account uses a COMMENT review beginning `REVIEWER VERDICT: APPROVED FOR FIXER/INTEGRATOR`. With hard defects use REQUEST_CHANGES if permitted, otherwise a COMMENT beginning `REVIEWER VERDICT: CHANGES REQUIRED`. Record review ID and reviewed SHA. A state field or ordinary comment never substitutes for an actual formal review.

Fixer closes the complete hard set, checks surrounding invariants and reruns applicable tests. With no requested fixes, perform a finishing inspection; only make an evidence-backed needed correction. Avoid optional churn to an already approved head. Mark each finding resolved with evidence and include final SHA. If any candidate content changed, clear prior readiness/review evidence for the old head and hand the exact new head to `REVIEW_READY` after CI; do not self-promote changed code directly to integration.

Integrator requires a real formal review on the final exact head (or an explicitly documented independent review of the entire post-review delta), all candidate-local hard findings resolved, final exact-head successful CI and no unresolved hard thread. Before merge, Integrator MUST perform one last live reconciliation after the latest review/check completion; a review thread created after state was written blocks integration immediately.

## Qualification and product progress

At the start of a new stroke or changed product authority, read the current product documents in full. Subsequent wake-ups inspect changed documents and the relevant current sections/code, rather than repeating an unchanged whole-project audit. Never trust stale remembered summaries.

Each package must finish a useful user workflow or a measurable protection of one, with golden-journey acceptance, failure/conflict/cancellation evidence where relevant and Release-effective tests. UI changes need actual GTK/interaction evidence and Day/Night/keyboard/focus consideration. Optional visual polish or unavailable screenshots alone do not stall this alpha loop; record the evidence gap. Reproducible correctness, accessibility, data-loss or recovery defects remain HARD. Track feature maturity and remaining gaps in the roadmap and truthful application docs. Release count is an output, not proof of completeness.

After a safety/correctness release, ordinary documented CORE capability must continue to advance unless a live P1/P2 defect directly blocks that work. Safety-only follow-ups do not silently replace an explicit user-feature tranche. If a safety defect is found while implementing the active feature tranche, fix it within the same coherent candidate where appropriate and then continue the requested feature scope.

## Release and recovery

Integrator records integration SHA, version, release SHA, workflow IDs and publication status before crossing each external boundary. Reconcile actual main/VERSION/release tag before retrying so a lost report cannot cause another bump. Merge with expected-head protection, then create a tested `Release X.Y.Z` commit and use the repository's normal immutable release workflow. No empty product release for a counter.

RELEASED requires the exact GitHub tag/release, expected assets, successful release-SHA verification AND both public central APT catalogue/apps.json and beta amd64 Packages reporting that exact Files version. If APT lags, use PUBLICATION_PENDING, release the lease and resume the SAME version. When authorized and needed, rerun the existing central publish job or use its documented refresh path; never modify unrelated applications.

Missing state alone does not authorize new work: reconcile the current PR/release first. Never infer rollback safety or completed publication from old reports. A genuine blocker is an overlay {kind, action, reason, since, next_action}; preserve the resumable phase and candidate.

## Communication and health

Every run records a heartbeat for its own role with at, status, observed revision/head and precise next action, as part of a safe state update without displacing another lease. A timer `last_run_time` is NOT proof of a healthy worker. Health is established by durable GitHub heartbeat/reconciliation evidence.

Every worker must reconcile live GitHub before recording an unchanged/no-op heartbeat. If live evidence contradicts state, repairing state is itself the first material action of the run. A worker must never report "nothing to do" while an active PR has unresolved HARD review threads, a changed head with stale review/CI, a merged candidate still marked open, or publication evidence newer than state.

After a material change publish state FIRST and a concise issue #2 audit comment SECOND: role / stroke / phase / PR@SHA / change / tests / product impact / risks / next. Include PHILOSOPHY/CRAFT drift or evidence briefly. Unchanged observations need only heartbeat, not another essay.

Read back external writes; say committed/posted/reviewed/merged/released only after confirmation. For transient transport/service errors (including a reported MCP Internal error without an explicit permission/approval denial), first read actual remote state to determine whether the write took effect. If absent, retry the SAME authorized action at most twice with short backoff (each wait at most 45 seconds), using the same stroke/nonce and current expected SHA. Recover an existing result rather than duplicate it. After exhausted retries preserve the resumable checkpoint and report the exact service failure. Never classify an unspecified internal error as a proven safety/permission denial. If an approval or permission rejection occurs, stop that rejected action, retain the current phase, report the exact action and stated reason in the automation response, and keep workers enabled. Never retry through another transport to bypass a restriction. A failed audit comment cannot erase an already confirmed state handoff; a missing formal review still blocks integration.

Integrator flags either (a) a missing heartbeat after two expected hourly runs OR (b) a reconciliation watermark older than two expected hourly runs while the candidate has live activity. In either case it checks automation enabled/error status when exposed and attempts authorized routine recovery. Changes to worker instructions/schedules require BEFORE / AFTER / WHY / EVIDENCE / ROLLBACK in issue #2 and a saved prior configuration. Never silently disable a worker.

## Startup communication check

While mode is VERIFY_HANDOFFS, make no application/PR/release/automation changes. Run in order Architect, Builder, Reviewer, Fixer, Integrator using health.check_order. Require the same health.nonce and read the preceding role's receipt from GitHub; do not invent it.

Post one issue #2 comment containing `FILES SWARM CHECK <nonce> <Role>`, the actual previous receipt/comment ID (or "first" for Architect), baseline main SHA and "No product changes". Read back the comment. Atomically append only your own receipt {role, nonce, at, previous_role, previous_comment_id, comment_id, observed_revision, observed_main_sha} to health.receipts, add your heartbeat, increment revision and read back. Duplicate runs validate the existing receipt instead of duplicating it. If the nonce/role comment already exists but the receipt write failed, recover its actual comment ID rather than posting it again. If the predecessor is not yet present, perform bounded read-only state rechecks for up to three minutes with waits no longer than 45 seconds where available; never post or claim a receipt before observing the actual predecessor. On timeout or a rejected write, report the precise failure and leave mode unchanged. Integrator leaves the mode unchanged; HAL verifies all five independent receipts before switching to WORK.
