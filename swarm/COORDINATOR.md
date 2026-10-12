# Files swarm event coordinator

The Files swarm uses GitHub as the durable control plane. The five ChatGPT workers remain the role executors, but their hourly schedules are fallback wake-ups rather than the authoritative baton mechanism.

## Why this exists

The original timer-only design could lose causality: a Fixer could write `REVIEW_READY`, then an asynchronous review could open a new HARD thread minutes later. The stored phase stayed stale until a worker happened to notice it. That made a healthy timer look like a healthy swarm even while GitHub reality required different work.

The event coordinator closes that gap. `.github/workflows/swarm-coordinator.yml` lives on `main` so GitHub can invoke it for repository events. Its implementation and tests live on `swarm/control` in `swarm/coordinator.py`, `swarm/coordinator_entry.py`, `swarm/test_coordinator.py` and `swarm/test_coordinator_security.py`.

## Events and fallback

The coordinator runs after:

- same-repository pull-request open/reopen/synchronize/close/ready-for-review events;
- same-repository pull-request review submission/edit/dismissal events;
- same-repository inline review comment create/edit/delete events;
- completion of the `Verify` workflow;
- explicit workflow dispatch;
- a 15-minute GitHub Actions fallback reconciliation.

Review-thread resolution itself does not always emit a dedicated Actions event, but a Fixer reply and the subsequent exact-head Verify completion both cause reconciliation. The 15-minute fallback also closes any event gap before the worker timers are needed.

## Authority and trust boundary

The coordinator is deliberately not an AI worker. It must never implement product code, judge architecture, write a review, resolve a review thread, merge, version, release or publish.

This is a public repository, so arbitrary public comments and approvals are not authority. `coordinator_entry.py` applies the trust policy before live reconciliation:

- protocol review verdicts are accepted only from configured trusted reviewer accounts (currently `The-First-Infiltrator`);
- HARD inline evidence is accepted only from trusted reviewer accounts or the repository's configured Codex review bot;
- an arbitrary public `P1`, `P2`, `HARD` comment or approval cannot move swarm state;
- fork-PR events do not receive the writable coordinator job.

It then converts trusted objective GitHub facts into deterministic routing facts in `swarm/state.json`:

- live PR/head replaces stale candidate-head data;
- an unresolved current trusted P1/P2/HARD thread forces `FIX_REQUIRED`;
- an exact-head trusted `REVIEWER VERDICT: CHANGES REQUIRED` forces `FIX_REQUIRED`;
- exact-head green Verify plus an exact-head trusted protocol approval can reconstruct `INTEGRATE_READY` if a worker handoff was lost;
- a changed head invalidates readiness tied to the previous SHA;
- an already merged active candidate routes to Integrator reconciliation rather than allowing a new stroke.

Forward product decisions that are not mechanically proven remain owned by the five workers.

## Concurrency and durability

State updates use the GitHub Contents API with the current blob SHA. A stale write is refetched and reconciled up to three times; the coordinator never force-writes an old snapshot over a newer worker update. Every successful write is read back before it is reported as complete.

The state gains a `stroke.reconciliation` watermark containing the event, current main, candidate head/state, exact-head CI, exact-head review and complete unresolved trusted HARD thread set. Phase-changing reconciliations also leave a concise issue #2 audit comment.

Coordinator runs are serialized with a GitHub Actions concurrency group. Updating `swarm/control` does not itself retrigger the coordinator, preventing feedback loops.

## Worker relationship

Workers still independently re-read live GitHub at every wake. The coordinator is an event-driven safety router, not a substitute for role reasoning. If a worker sees facts newer than the reconciliation watermark, live GitHub still wins and the worker repairs state before role work.

This gives three resilience layers:

`GitHub event -> deterministic coordinator -> state/baton`

then, if an event is missed:

`15-minute GitHub fallback -> deterministic coordinator -> state/baton`

and, as execution/recovery:

`hourly worker wake -> independent reconciliation -> role execution`.

The coordinator cannot directly invoke a ChatGPT automation; the five workers remain the execution layer. The benefit is that when the correct worker wakes, the baton is already deterministic and current instead of depending on a previous timer's stale handoff.

## Tests

`swarm/test_coordinator.py` covers the core routing invariants: unresolved HARD findings force Fixer, exact-head approval plus green CI reconstructs integration readiness, a changed head invalidates old readiness, resolved/outdated comments do not block, and an already merged candidate routes to Integrator reconciliation.

`swarm/test_coordinator_security.py` proves that arbitrary public HARD comments and approvals cannot steer the swarm while trusted Reviewer/Codex evidence still can.

The workflow runs the full coordinator test set before every live reconciliation. If any test fails, state is not mutated.
