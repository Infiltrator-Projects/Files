# Files — Product Quality Contract

**Purpose:** define how Files proves that it is becoming a better file manager, not merely a larger or more technically elaborate repository.

This document sits alongside `DESIGN.md`, `ARCHITECTURE.md`, `FEATURES.md`, `COMMON-INTEGRATION.md` and `RESEARCH-2026.md`.

`FEATURES.md` describes the intended product surface. `DESIGN.md` describes the product philosophy and quality bar. This document defines the acceptance discipline used to decide whether an implementation tranche actually improves Files for a human being.

The governing question is:

> **Did this change make Files more useful, trustworthy, effortless, beautiful and maintainable without disproportionate complexity?**

A green build is necessary. It is not sufficient.

---

## 1. Three independent truths

Every meaningful tranche is judged against three independent forms of evidence.

### Engineering truth

Files must be correct, recoverable, performant, maintainable and architecturally coherent.

Evidence may include:

- compile/test/CI results;
- deterministic success/failure/recovery behaviour;
- operation verification and journal evidence;
- memory/allocation and latency measurements;
- large-directory, removable-media and remote-location behaviour;
- cancellation and interruption behaviour;
- capability/provider boundary correctness;
- code structure, ownership and lifetime clarity;
- documentation and release consistency.

### Interaction truth

A person must be able to accomplish ordinary file work naturally and confidently without understanding the implementation.

Evidence may include:

- golden-journey walkthroughs;
- keyboard-only execution;
- focus and selection behaviour;
- task-step count and unnecessary mode switching;
- clear operation consequences and recovery paths;
- understandable conflict/error/progress states;
- preservation of context during navigation and search;
- absence of shell freezes or inexplicable state changes.

### Aesthetic truth

The running product must look and feel intentionally designed, not merely technically compliant.

Evidence may include:

- rendered Day and Night screenshots or recordings;
- before/after visual comparison for UI changes;
- typography, spacing, proportion, alignment and hierarchy review;
- animation/transitions where present;
- density and information-balance review;
- hover, focus, selection, progress, conflict and error-state review;
- consistency with authoritative Common design contracts.

No worker may infer aesthetic success from source code alone when the change materially affects rendered UI.

---

## 2. Golden user journeys

These journeys are the product-level acceptance spine. They are intentionally broader than individual feature tests.

A release does not need every future feature in `FEATURES.md`, but the journeys touched by a tranche must remain coherent and trustworthy.

### GJ-01 — Open Files and become useful immediately

The application launches quickly, paints useful chrome before slow secondary work completes, and shows a meaningful initial location without avoidable synchronous delay.

The shell must remain responsive while directory enrichment, devices, thumbnails, network locations, indexes or history load.

### GJ-02 — Navigate deeply and return without losing context

The user can move through folders, parent, history and direct location entry while preserving sensible selection, scroll and navigation context.

Returning to a previous location should feel like returning to where the user was, not merely re-opening the same pathname.

### GJ-03 — Find a file when the location is known

The user can reach a known file quickly by browsing, path/breadcrumb interaction and keyboard navigation without unnecessary modal UI or toolbar hunting.

### GJ-04 — Find a file when only fragments are remembered

As search capability matures, the user can move from navigation into exact search and eventually richer retrieval without losing the connection to the real filesystem object and its containing location.

Search must augment navigation, not replace filesystem truth.

### GJ-05 — Create and organise ordinary files with low friction

Create, rename, duplicate/copy, move and basic organisation must feel direct, predictable and fast.

Naming restrictions and conflicts must be explained by the destination/provider rather than invented by arbitrary UI rules.

### GJ-06 — Transfer a large collection and understand what is happening

For large or multi-item work, the user can see meaningful progress, cancel at defined safe boundaries, understand what completed, and trust that an interrupted operation will not be reported as successful.

The UI must remain responsive.

### GJ-07 — Resolve conflicts confidently

Replace, Skip, Keep Both and future merge/compare choices must be explicit and understandable.

The system must never equate matching names with matching objects or content, and must never hide destructive consequences behind convenience wording.

### GJ-08 — Make a mistake and recover honestly

Trash, restore, replacement recovery and operation history must expose genuine recovery mechanisms where they exist.

Files must never manufacture an Undo promise when no reliable inverse exists.

### GJ-09 — Survive disappearing removable storage

If removable media disappears during browse or operation work, Files remains responsive, reports the actual state and leaves no false success state.

Recovery or retry is offered only where it is justified by evidence and identity.

### GJ-10 — Survive slow or broken remote locations

A dead or slow network/provider location must not freeze navigation chrome or the entire application.

Timeout, disconnect, reconnect and partial-state behaviour must be explicit and recoverable where possible.

### GJ-11 — Perform important work from the keyboard

The main browse, navigation, selection and ordinary operation flows must be usable without a mouse.

Focus order and shortcuts must be deliberate and visible enough to learn.

### GJ-12 — Feel deliberately designed in Day and Night modes

Both appearance modes must look intentional rather than being simple colour inversions.

Typography, contrast, spacing, hierarchy, hover, focus, selection, progress, conflict and error states must remain coherent in both.

---

## 3. The proportional-complexity rule

> **A small feature is not allowed to cause a disproportionately large increase in conceptual complexity.**

If an apparently small user-facing change requires broad unrelated edits, duplicated policy, new cross-layer dependencies, a large controller expansion or repeated special cases, stop and inspect the boundary before continuing.

A difficult feature may legitimately require substantial code. The warning sign is not line count by itself; it is conceptual spread.

The Scout and Reviewer should ask:

- How many unrelated components had to know about this feature?
- Did a UI concern leak into operation or provider semantics?
- Was policy duplicated because an abstraction boundary is missing?
- Did a new state transition become implicit rather than modelled?
- Is the feature easier to explain after the change than before it?
- Could the same requirement be met with less permanent machinery?

When complexity increases, the PR must explain why the increase is necessary and what keeps it bounded.

---

## 4. Vertical-slice rule

Prefer complete, user-meaningful slices over long stretches of invisible substrate work when architecture does not require otherwise.

A good tranche often contains:

```text
user need
  ↓
model/capability support
  ↓
interaction
  ↓
failure/recovery behaviour
  ↓
qualification
  ↓
visual refinement
  ↓
documentation
```

Examples of coherent slices include breadcrumb navigation, tabs, bookmarks/places, clipboard workflow, view/sort behaviour, properties, thumbnails and drag/drop.

Do not force a visible feature before its architectural dependency exists. Equally, do not build speculative frameworks far ahead of any proven user need.

---

## 5. Required evidence for UI-affecting work

Any PR that materially changes visible layout, controls, navigation composition, dialogs, progress, error/conflict presentation, selection, focus or theme behaviour should include rendered evidence when practical.

Preferred evidence:

- before/after screenshots;
- Day and Night states where appearance is affected;
- narrow and ordinary window widths for responsive work;
- relevant hover/focus/selection/error/progress states;
- short recording for transitions or interaction behaviour when a still image is insufficient.

If rendered evidence cannot be obtained, the PR and Reviewer handoff must explicitly state the gap. Lack of visual evidence does not automatically block non-visual core work, but it prevents claiming that visual polish has been proven.

---

## 6. Product-level measurements

Measurements should be introduced where they become practical and kept comparable over time.

High-value measurements include:

- process launch to first useful frame;
- first visible directory population latency;
- large-directory population time;
- UI/main-thread stalls during enumeration and operations;
- memory use for representative large directories;
- cancellation response latency;
- transfer throughput without sacrificing correctness;
- redundant metadata/enumeration work;
- source files/components touched per ordinary feature as an architecture-smell indicator;
- controller/component growth where it indicates responsibility creep.

Numbers are diagnostic evidence, not targets to game. A faster result that weakens recoverability, correctness, accessibility or maintainability is a regression.

---

## 7. Golden-journey impact statement

Every Builder PR must identify:

- which golden journey(s) it improves or protects;
- which journey(s) could regress;
- what evidence proves the intended improvement;
- what remains unproven.

A tranche that cannot identify any affected user journey must justify why it is necessary architectural or maintenance work.

---

## 8. Product-quality review questions

Reviewer and Integrator should ask these questions in addition to normal code review:

### Does it make Files better for a person?

- Is a common task faster, clearer, safer or more capable?
- Does the user retain context?
- Are consequences understandable before commitment?
- Does failure leave the user knowing what happened and what can be done next?

### Does it still feel like Files?

- Does it preserve first-principles design rather than competitor imitation?
- Is the primary surface calm and progressively disclosed?
- Does it respect the Mercedes-Benz standard of quality, trust, comfort and disciplined reduction?
- Is the design likely to age well rather than follow a short-lived visual fashion?

### Did engineering get better or worse?

- Is ownership clearer?
- Is policy more centralized or more duplicated?
- Did coupling increase?
- Did an existing controller become a god object?
- Are comments preserving important reasoning?
- Is the new behaviour testable without a GUI where appropriate?

### Did the swarm create bureaucracy instead of product value?

- Did workers spend more effort discussing process than improving Files?
- Did multiple workers repeat the same audit without new evidence?
- Did cadence produce empty or premature reviews?
- Did self-modification improve actual throughput, quality or decision accuracy?

If process activity rises while meaningful product progress falls, the Integrator should simplify the swarm.

---

## 9. Release-quality gate

A coherent release tranche should not be called complete until all applicable items are true:

1. intended behaviour is implemented;
2. exact intended SHA is qualified by CI;
3. affected golden journeys have evidence appropriate to the change;
4. failure, recovery and cancellation behaviour is qualified where relevant;
5. architecture/complexity review is clean or debt is explicitly recorded;
6. UI-affecting work has rendered evidence or a clearly recorded evidence gap;
7. accessibility and keyboard impact has been considered;
8. README/design/feature documentation remains truthful;
9. Git history and branches are clean enough to understand the tranche;
10. GitHub release and central APT publication are verified when the release process requires them.

---

## 10. Swarm success criteria

The swarm experiment is successful only if it measurably helps produce a better File Manager.

Signs of success:

- coherent small PRs;
- fewer regressions escaping review;
- useful disagreement between workers;
- faster discovery of architectural drift;
- visible user capability arriving in sensible dependency order;
- decreasing repeated work;
- evidence-driven cadence or role adaptation;
- improving golden-journey quality;
- no loss of design/philosophy fidelity.

Signs of failure:

- issue #2 becomes mostly discussion about the swarm itself;
- workers repeatedly restate the same findings;
- prompts grow while product progress slows;
- PRs optimize metrics rather than human experience;
- review becomes ceremonial;
- agents agree without independent evidence;
- the repository becomes more elaborate while ordinary file-manager workflows remain weak.

When these failure signs appear, simplify the process before adding more agents or more automation.

---

## 11. Product-quality verdict format

Every meaningful Scout, Builder, Reviewer and Integrator handoff should include a short product verdict answering:

```text
PRODUCT IMPACT
Golden journey(s):
User-visible improvement:
Engineering impact:
Visual/interaction evidence:
Complexity impact:
What remains unproven:
Did this make Files better? YES / NO / UNCLEAR
```

`UNCLEAR` is acceptable when evidence is genuinely missing. It is better than inventing confidence.

---

## 12. Principle

The goal is not to finish the catalogue as quickly as possible.

The goal is to produce a file manager that people can trust with their files, understand without effort, enjoy using every day, and maintain for years without architectural decay.

Feature count, test count, commit count and swarm activity are means. **Product quality is the end.**
