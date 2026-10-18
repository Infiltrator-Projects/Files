# Files — Common Integration and Ownership Ledger

**Date:** 18 October 2026\
**Pinned Common:** 1.19.63\
**Pinned commit:** `ae38474d162e08afce06e234ccb0f8e00f406c85`

Common is one of Files's principal architectural advantages. Files must not treat Common as a token submodule used only for colours. It is the first place to look whenever Files needs a product-neutral mechanism already proven elsewhere in the Infiltrator family.

The rule is not “put everything in Common.” The rule is:

> **Use Common whenever its existing contract is at least as strong as the best Files implementation we would otherwise write. Keep file-manager semantics in Files and filesystem-specific semantics in their filesystem. Promote new shared mechanics only when Common's own admission rules are satisfied.**

This ledger records the intended boundary.

---

# 1. Why Common is a strategic advantage

A conventional clean-sheet file manager normally begins by accumulating local helpers for configuration, paths, formatting, timing, localisation, theme constants, string handling, plugin loading, safe arithmetic and state persistence. Those helpers then diverge from the rest of the operating system.

Files does not need to start there.

Common already provides qualified, tested first-party contracts used by System Monitor, Calendar, Defragger, InfiltratorFS, Link and other products. Files can therefore start with a consistent Infiltrator foundation and concentrate engineering effort on the genuinely file-manager-specific problems: object/location modelling, directory presentation, operations, recovery, retrieval, capability discovery and user interaction.

Common also gives Files cross-product consistency automatically where consistency is desirable:

- the same System/Day/Night meaning;
- the same typography identity;
- the same spacing/radius vocabulary;
- the same system-wide temporal presentation authority;
- the same deterministic parsing rules;
- the same checked arithmetic behaviour;
- the same product identity/build vocabulary;
- the same localisation engine;
- the same durable state-publication mechanics on POSIX;
- the same dynamic-library binding rules; and
- the same engineering expectations and regression coverage.

That is the “leg up”: Files starts above the generic-infrastructure layer rather than recreating it.

---

# 2. Dependency rule

Files pins an exact reviewed Common release and commit. It must not silently float to Common `main` at build or runtime.

Current requirement:

```text
Common version: 1.19.63
Commit:         70452491dae6d0f9f75c7099292f624db08648cc
```

A Common update is an explicit Files change with build/test qualification. A same-version but different Common commit must not build in a normal Git checkout.

Files should link the strongest relevant Common target rather than enumerate Common implementation source files:

```text
InfiltratrCommon::Portable   toolkit/OS-neutral algorithms
InfiltratrCommon::Common     portable algorithms + current platform provider
InfiltratrCommon::Shared     shared-library form when that deployment model is required
```

For the InfiltratorOS/Linux desktop build, Files should link `InfiltratrCommon::Common`, because the product needs Common's POSIX path/state/I/O contracts as well as the portable design and formatting layer.

---

# 3. Common capabilities Files should consume

## 3.1 Project identity and build metadata

**Common owns:**

- `InfiltratrProjectInfo`;
- validation of stable project metadata;
- canonical build-profile labels.

**Files uses it for:**

- About/build diagnostics;
- support evidence;
- machine-readable `--version`/diagnostic output where appropriate;
- consistent release/build identity.

**Files still owns:**

- Files product name and executable/application ID;
- Files-specific description/icon;
- release policy.

Status: **REQUIRED**.

---

## 3.2 Theme, design metrics and typography

**Common owns:**

- System/Day/Night theme policy;
- semantic colour roles;
- canonical design spacing/radius metrics;
- MB Corpo UI/brand family identity and role weights;
- canonical first-party font asset provenance.

**Files uses it for:**

- window/surface/panel/input/selection/status colours;
- hover/fault/warning/success states where roles fit;
- standard control/card/panel radii;
- standard content/screen spacing;
- canonical typography identity;
- system theme persistence keys and parsing when Files owns a local preference.

**Files still owns:**

- file-manager-specific geometry;
- iconography;
- view density;
- navigation composition;
- operation-progress visualisation;
- domain-specific accent/selection decisions not represented by neutral roles.

Rule: Files must not copy Common palette values into local constants and let them drift.

Status: **ALREADY ACTIVE**.

---

## 3.3 String and deterministic ASCII mechanics

**Common owns:**

- bounded string copy/trim;
- start/end/equality operations;
- deterministic ASCII classification;
- case conversion;
- case-insensitive equality, prefix, containment and lexical comparison.

**Files uses it for:**

- preference/configuration keys;
- provider identifiers;
- internal protocol/capability identifiers;
- deterministic ASCII command/extension matching;
- places where locale-sensitive filename comparison would be incorrect.

**Files must not use it for:**

- pretending arbitrary Unicode filenames are ASCII;
- locale-aware human filename sorting where Unicode collation is intended.

Filename presentation/search requires a separate Unicode-aware contract at the application/platform layer. Common's deterministic ASCII family is for identifiers and grammars, not a substitute for Unicode text handling.

Status: **REQUIRED WHERE APPLICABLE**.

---

## 3.4 Checked/saturating arithmetic

**Common owns:**

- checked integer arithmetic;
- saturating arithmetic;
- checked `size_t` arithmetic / array reservation;
- safe percentage and quantity mechanics.

**Files uses it for:**

- byte totals;
- recursive-size accumulation;
- progress numerator/denominator calculations;
- buffer and allocation sizing;
- batch-operation counts;
- thumbnail/cache sizing;
- index sizing;
- tree walking and aggregate metadata where overflow must not become a false value.

Rule: overflowing a file-size/progress calculation produces an explicit unavailable/overflow state, never a wrapped number displayed as truth.

Status: **REQUIRED**.

---

## 3.5 Stable non-cryptographic identity/signature hashing

**Common owns:** stable FNV-1a byte/text/u64 mixing.

**Files uses it for:**

- cheap runtime change signatures;
- cache keys where collision is not a security boundary;
- provider/model fingerprint composition;
- stable in-process or persisted non-security signatures when the composition contract is explicitly Files-owned.

**Files must not use it for:**

- content integrity verification;
- adversarial identifiers;
- authentication/security.

Cryptographic content hashes remain a separate qualified implementation/provider.

Status: **AVAILABLE / USE WHEN JUSTIFIED**.

---

## 3.6 UTF-8 primitives

**Common owns:**

- strict UTF-8 validation;
- Unicode-scalar-to-UTF-8 encoding.

**Files uses it for:**

- validating Files-owned UTF-8 state/catalogue data;
- validating provider text contracts that require canonical UTF-8;
- emitting scalar values into UTF-8 without local encoders.

**Files still owns:**

- filesystem/provider policy for raw/non-UTF-8 names;
- display escaping;
- bidi/spoofing presentation policy;
- platform filename conversion.

Rule: Files must not corrupt or reject a filesystem object merely because its platform-native name cannot be represented by one simplistic internal assumption. The toolkit-neutral model requires a deliberate filename encoding boundary.

Status: **REQUIRED FOR FILES-OWNED UTF-8 CONTRACTS**.

---

## 3.7 Quantity and file-size formatting

**Common owns:** generic quantity scaling and canonical disk-capacity formatting.

**Files uses it for:**

- file size;
- directory aggregate size;
- volume capacity/free/used display;
- operation byte totals;
- allocated/logical size display when units are the same class.

**Files still owns:**

- wording such as `logical`, `allocated`, `shared`, `estimated`;
- semantic distinction between apparent size and physical storage cost.

Rule: one byte count should not render differently in Files, System Monitor and other Infiltrator applications merely because each application wrote its own unit helper.

Status: **ACTIVE**. Detail/List file sizes use Common's canonical disk-capacity formatter; other quantity surfaces should use the same authority when introduced.

---

## 3.8 Scalar/metric formatting

**Common owns:** fixed-point ASCII and general scalar formatting plus several proven metric renderers.

**Files uses it for:**

- machine-readable Files-owned state;
- percentages/progress labels where the Common presentation contract fits;
- consistent unavailable-value semantics.

**Files still owns:** file-manager-specific status sentence composition.

Status: **USE WHERE CONTRACT FITS**.

---

## 3.9 Configuration parsing

**Common owns:**

- allocation-free `key=value` line parsing;
- conservative shared boolean parsing.

**Files uses it for:**

- simple Files-owned text preferences/state where that format remains appropriate;
- deterministic parser behaviour independent of GTK/GLib.

**Files still owns:**

- preference schema;
- defaults;
- migration;
- validation of product-specific values;
- deciding whether a particular state belongs in Files at all.

Rule: system-wide settings already owned by System Settings/Common are read from the shared authority rather than copied into a private Files preference.

Status: **REQUIRED IF FILES USES KEY/VALUE STATE**.

---

## 3.10 POSIX home and XDG path resolution

**Common owns:**

- current-user home resolution;
- XDG config home;
- XDG data home;
- recursive directory creation;
- generic path joining/concatenation/dirname mechanics.

**Files uses it for:**

- Files-owned configuration/state/index roots on InfiltratorOS;
- portable non-UI POSIX core code;
- removing unnecessary GLib dependence from the model/operation layers.

**GTK/GIO may still own:**

- special user directories exposed through the desktop platform contract;
- URI/location objects in the Linux UI/provider layer.

Status: **REQUIRED IN NON-UI POSIX STATE CODE**.

---

## 3.11 Exact POSIX I/O

**Common owns:**

- exact EINTR-safe sequential reads/writes;
- exact positioned reads/writes;
- bounded and allocated complete-file readers;
- typed numeric file readers;
- rich I/O result categories.

**Files uses it for:**

- Files-owned operation journal/state files;
- Files-owned caches/index metadata where a raw POSIX file contract is appropriate;
- provider/platform support code that otherwise would recreate exact-I/O loops.

**Files does not use raw POSIX I/O as a substitute for GIO/provider semantics** when operating on generic browseable objects. Remote/MTP/trash/virtual locations still require the location/provider abstraction.

Status: **REQUIRED FOR FILES-OWNED LOCAL STATE; SELECTIVE ELSEWHERE**.

---

## 3.12 Durable atomic publication and removal

**Common owns:**

- durable atomic file replacement;
- durable namespace removal for Files-owned POSIX state.

**Files uses it for:**

- preferences;
- operation journal checkpoints;
- bookmark/index metadata where local Files state must survive interruption;
- small state snapshots that should never be left half-written.

**Files must not confuse this with user-file operation semantics.** Copy/move/delete of arbitrary user objects go through the deterministic operation engine and their provider/filesystem contracts. Common's generic durable file helper is not permission to bypass provider semantics.

Status: **REQUIRED FOR FILES-OWNED DURABLE STATE**.

---

## 3.13 Timing and monotonic clocks

**Common owns:** exact timing/periodic cadence and POSIX monotonic timing/deadline mechanics.

**Files uses it for:**

- operation elapsed/rate calculations;
- debounce/coalescing intervals where toolkit-independent logic is preferable;
- benchmark/qualification instrumentation;
- retry/backoff timing;
- activity-window timestamps that require monotonic intervals rather than wall-clock subtraction.

**GTK main-loop scheduling still owns** actual UI callback dispatch where that is the correct platform mechanism.

Status: **REQUIRED IN OPERATION/MODEL LOGIC**.

---

## 3.14 System-wide temporal presentation

Common already carries the shared system-wide temporal presentation authority used by System Settings and Calendar.

**Common owns:**

- current temporal presentation policy schema;
- clock/calendar catalogues;
- seconds policy;
- location-dependent clock policy;
- executable system Clock Resources discovery/validation and shared presentation;
- optional bounded POSIX Calendar Resources discovery and validation without an application dependency.

**Files uses it for:**

- displayed file timestamps following the user's system-wide Infiltrator presentation choices where semantically appropriate;
- consistent date/time vocabulary across the OS;
- avoiding a private “12/24-hour” preference disconnected from System Settings.

**Files still owns:**

- relative descriptions such as “Today”/“Yesterday” if used;
- choosing which file timestamps to show;
- date column layout;
- timezone/source timestamp semantics.

Rule: Files should not invent another clock/calendar configuration system.

Status: **ACTIVE**. One policy snapshot supplies the date and clock for each displayed timestamp. Common 1.19.63 requires native .so resources for all eighteen extracted modes (sixteen alternative clocks and explicit 12/24-hour time). An absent, invalid or mismatched resource fails closed; Files no longer has an old compiled fallback for Julian Date, Modified Julian Date, Chinese hundred-kè or Mean Solar. Non-Gregorian dates use the Common-owned Calendar Resources ABI for matching UI-language presentation and native historical year conventions; a missing/incompatible provider is shown as unavailable. Gregorian locale presentation stays with the platform adapter. Live policy refresh and raw timestamp sorting share the existing row-formatting seam. A fresh profile watches the nearest existing configuration ancestor until the shared policy directory appears. Native locale time patterns are converted to UTF-8 before GTK formatting.

---

## 3.15 Localisation

**Common owns:**

- locale normalisation;
- immutable catalogue lookup;
- canonical fallback order;
- named placeholder interpolation.

**Files uses it for:**

- all product strings once localisation begins;
- operation/error/status text templates;
- plural-aware product layer built on top of an explicit Files contract where required.

**Files owns:**

- actual translations/catalogues;
- right-to-left UI/layout decisions;
- file-manager terminology.

Status: **REQUIRED BEFORE LOCALISED RELEASES**.

---

## 3.16 Dynamic library binding

**Common owns:**

- portable module lifetime;
- required/optional symbol lookup;
- atomic symbol-table binding across POSIX and Win32.

**Files uses it for:**

- qualified optional native providers when an out-of-process service is not the stronger design;
- preview/metadata/capability modules that have an explicit versioned ABI.

**Files owns:**

- provider ABI;
- provider names/version probing;
- capabilities;
- sandbox/isolation policy;
- provider failure handling.

Rule: Common may load the library; Files defines what a “file manager provider” means.

Status: **ADVANCED**.

---

## 3.17 Escaping/URI output

Common owns generic HTML/JSON/URI-component escaping.

**Files uses it for:**

- exported diagnostics or generated JSON/URI-component text where Common's escaping contract fits.

**Files does not use generic URI escaping to replace GFile/GIO location semantics.** A browseable URI is not merely a string-escaping problem.

Status: **SELECTIVE**.

---

## 3.18 Graphics primitives

Common owns software surfaces, alpha composition and nearest/bilinear scaling.

Potential Files use:

- toolkit-neutral thumbnail/preview transforms if measurement and provider architecture show a benefit;
- deterministic test rendering.

GTK/GDK may remain the stronger path for many desktop rendering operations. Common graphics should be used because its contract is better for the task, not merely because it exists.

Status: **AVAILABLE / NOT MANDATORY**.

---

## 3.19 Endian primitives

Common owns fixed-width byte-order mechanics.

Files normally should not parse filesystem or archive binary formats itself merely to use these helpers. If a Files-owned provider genuinely parses a binary metadata protocol and Common endian operations fit, it should use them.

Status: **AVAILABLE / RARE IN FILES CORE**.

---

# 4. Capability-to-owner matrix

| Capability | Common | Files | Platform/provider | InfiltratorFS |
| --- | --- | --- | --- | --- |
| Theme palette/metrics/typography | **Authority** | composition | native rendering | — |
| Project/build identity mechanics | **Authority** | product values | — | — |
| Checked arithmetic | **Authority** | domain use | — | — |
| File-size quantity formatting | **Authority** | semantic label | — | — |
| Localisation engine | **Authority** | strings/catalogue | platform locale source | — |
| System-wide time presentation | **Authority** | file-time composition | OS locale/timezone adapter | — |
| Selected-calendar date rendering | **discovery/validation** | file-date composition | **Calendar chronology; OS local civil date** | — |
| Home/XDG/path helpers | **Authority on POSIX mechanics** | state layout | OS provider | — |
| Files-owned durable state I/O | **generic mechanics** | schema/recovery policy | POSIX provider | — |
| Generic user-file browsing | — | object/location model | **I/O namespace provider** | provider may expose more |
| Copy/move/delete semantics | supporting arithmetic/timing/I/O only | **operation authority** | execution capabilities | stronger storage semantics |
| Filesystem object identity | — | consumes capability | generic identity if available | **IFS authority** |
| Reflink/history/integrity | — | presents/requests | provider bridge | **IFS authority** |
| Search index | generic mechanics only | **authority** | file-content providers | stable IDs improve it |
| Semantic index | generic mechanics only | **authority, derived only** | model provider optional | stable IDs improve it |
| Plugin loading | **loader mechanics** | ABI/capability policy | shared-library platform | — |
| Archive formats | endian/checked primitives if needed | UX/operation model | archive provider | — |

---

# 5. Common integration required in the first implementation tranches

## Tranche A — browsing foundation

Files should already use Common for:

1. exact pinned dependency;
2. `InfiltratrCommon::Common` on the InfiltratorOS desktop build;
3. theme palette;
4. design metrics;
5. typography;
6. file/disk size formatting;
7. project identity/build metadata;
8. home/XDG resolution in non-UI state code;
9. checked arithmetic in new aggregate calculations.

Current 0.1.x gaps to remove:

- file-size formatting and the full POSIX Common build target are already active; preserve those choices in new surfaces;
- Files should add its `InfiltratrProjectInfo` record early so diagnostics/release identity are consistent from the beginning.

## Tranche B — deterministic operation engine

Use Common for:

- checked byte/count arithmetic;
- monotonic timing/rate mechanics;
- Files-owned journal state publication;
- exact local state I/O;
- generic path/state directory mechanics;
- parsing/formatting of persisted operation metadata where contracts fit.

Do **not** push operation semantics into Common. Copy/move/trash/delete planning, conflict rules, cancellation, verification and recovery are Files product semantics.

## Tranche C — search/index

Use Common for:

- checked allocations/counts;
- stable non-security signatures where appropriate;
- deterministic config identifiers;
- exact/durable local index metadata publication;
- monotonic timing/instrumentation.

Do not put semantic embeddings, file-ranking policy or activity semantics into Common.

## Tranche D — providers/extensions

Use Common's dynamic-library binder for native provider loading if an in-process provider is retained after safety evaluation. Files owns the ABI/version/capability contract.

---

# 6. New Common API policy for Files

Files must not add speculative helpers to Common simply because it is a new consumer.

A proposed Common addition from Files should satisfy Common's existing admission discipline, normally one of:

1. Files needs a product-neutral capability another Infiltrator product already implements privately;
2. Files duplicates a product-neutral capability already present elsewhere;
3. the new operation robustly completes an already-active Common family; or
4. real Files production code can be immediately replaced by the proposed Common operation.

Candidate future promotions should be discovered from implementation, not invented in advance.

Examples that **might** later qualify if another consumer needs the same mechanics:

- generic bounded operation-rate estimator;
- generic cancellable worker-state primitive;
- generic content-hash wrapper if multiple products converge on the same cryptographic contract;
- generic user-state schema/version helper.

None belongs in Common merely because Files could use it.

---

# 7. What must remain outside Common

The following are Files-specific and should not migrate to Common without a genuine second product-neutral consumer/contract:

- file/folder/location object model;
- navigation history;
- breadcrumbs;
- directory sorting/grouping policy;
- selection model;
- copy/move/delete plans;
- conflict resolution;
- Trash semantics;
- operation journal schema;
- thumbnail cache policy;
- search ranking;
- activity/workflow model;
- semantic retrieval policy;
- file-manager keyboard shortcuts;
- sidebar/place composition;
- tabs/split-view/session state;
- provider capability vocabulary if only Files consumes it.

The following are InfiltratorFS-specific and belong to InfiltratorFS or a stable InfiltratorFS user-space API, not Common:

- persistent filesystem object IDs;
- retained generations/history semantics;
- reflink implementation semantics;
- integrity/checksum trees;
- allocation/placement/compression policy;
- checkpoint/recovery details;
- protection/redundancy classes.

Common can supply generic mechanics used by those interfaces, but it must not become a second filesystem-policy layer.

---

# 8. Design test

For every new Files subsystem, implementation review should ask in this order:

1. **Does Common already own a strong product-neutral contract for this mechanic?** If yes, use it.
2. **Does the platform/provider own this mechanism more correctly?** If yes, adapt it behind the Files boundary.
3. **Is this actually Files product semantics?** If yes, Files owns it.
4. **Is this InfiltratorFS/filesystem policy?** If yes, consume a stable capability instead of copying it.
5. **Are we about to write a local generic helper that already exists elsewhere in the Infiltrator family?** If yes, stop and audit Common first.

That ordering is intentional. It is how Files avoids becoming another large, brittle island while still retaining ownership of the behaviour that makes a file manager a file manager.
