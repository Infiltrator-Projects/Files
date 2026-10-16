# Files — Research Review 2026

**Research date:** 6 October 2026

This document records research used to shape the clean-sheet design of Files. Research is evidence, not specification. The project retains authority over its own semantics and architecture.

The review concentrates on five questions:

1. how people actually retrieve and organise files;
2. what semantic/LLM retrieval can add without replacing the filesystem;
3. how context and workflow can reduce re-finding effort;
4. how recovery and reversibility should affect file operations; and
5. what interface structures can expose substantial capability without permanent complexity.

## 1. Navigation remains a first-class human strategy

### File hyper-searching explained (2024)

A controlled study compared 50 "hyper-searchers" with 50 controls. In the retrieval task, hyper-searchers used search for 67% of retrievals while the control group used it for only 5%. Hyper-searchers also had less organised file collections and a much higher navigation-failure rate (23% versus 6%).

Source: Bergman et al., *File hyper-searching explained*, Human–Computer Interaction, 2024.
https://doi.org/10.1080/07370024.2024.2379838

### Design consequence

There is no single correct retrieval mode.

A next-generation file manager should support at least:

- conventional folder navigation;
- precise filename/metadata/content search; and
- semantic/contextual search.

The interface should let users move between these modes without changing the underlying organisation of their files. Search should not require people to abandon folders, and good navigation should not make search a second-class feature.

## 2. Semantic file retrieval is now technically credible

### From Commands to Prompts: LLM-based Semantic File System for AIOS (ICLR 2025)

LSFS explores semantic file retrieval, summarisation, grouping and rollback using semantic indexes and natural-language interaction. The authors report at least a 15% retrieval-accuracy improvement and 2.1× retrieval speed in their semantic retrieval benchmark compared with their traditional comparison, while also retaining strong keyword retrieval.

Source: Shi et al., ICLR 2025.
https://proceedings.iclr.cc/paper_files/paper/2025/hash/517eb19e99947f60afff0cf93e451825-Abstract-Conference.html

### Design consequence

Natural-language retrieval and semantic indexing are worth taking seriously, but the research does **not** require the semantic system to become the filesystem authority.

For Files the stronger architecture is:

```text
real filesystem namespace = authoritative
semantic index            = rebuildable derived accelerator
natural-language query    = optional retrieval interface
mutation                   = deterministic operation engine
```

This preserves ordinary files, interoperability and recoverability while gaining the useful parts of semantic retrieval.

## 3. Context can make semantic retrieval substantially stronger

### OmniQuery (CHI 2025)

OmniQuery investigated natural-language questions over personal captured memories. Rather than relying only on isolated items, it augmented memories with contextual relationships across other captured information. In human evaluation it achieved 71.5% accuracy and won or tied against a conventional RAG baseline 74.5% of the time.

Source: Ma et al., *OmniQuery: Contextually Augmenting Captured Multimodal Memories to Enable Personal Question Answering*, CHI 2025.
https://doi.org/10.1145/3706598.3713448

### Design consequence

For files, semantic similarity alone is probably insufficient. Useful retrieval context may include:

- folder/location;
- creation and modification chronology;
- files opened or modified in the same activity window;
- explicit collections/bookmarks;
- application/project association;
- shared content/topic;
- version/history relationship;
- stable object identity across rename where available; and
- user-confirmed relationships.

Any relationship used to surface a file should be explainable to the user.

## 4. Information management is workflow-dependent

### An Empirical Study on Personal Information Management Practices Across Scholarly Workflows (CHI 2026)

This 2026 study frames personal information management across the full scholarly workflow rather than as a single storing/finding task. Its result supports designing systems around evolving information-management needs as work moves through stages.

Source: Xu & Signer, CHI 2026 Extended Abstracts.
https://doi.org/10.1145/3772363.3798664

### From Tabs to Structures: Understanding and Supporting Web Page Management (CHI 2026)

A field experiment with 29 knowledge workers examined how people manage evolving collections of web information. The work argues that rigid bookmark-like structures impose management cost and explores support for emerging, dynamic relationships between information items.

Source: CHI 2026.
https://doi.org/10.1145/3772318.3791979

### Design consequence

A file manager should not assume that the only meaningful relationship between files is their parent directory.

Without changing the real filesystem hierarchy, File Manager can eventually support derived views such as:

- current activity/project context;
- files recently used together;
- related items;
- temporary working sets;
- explicit user collections; and
- history/version relationships.

These views should be overlays, never replacements for the real namespace.

## 5. Recovery is not an edge case

### Achieving Resilience: Data Loss and Recovery on Devices for Personal Use in Three Countries (CHI 2025)

A survey of 1,423 people across Germany, the UK and the USA found that 46% had experienced at least one data-loss incident. Among people who recovered using backups, more than half reported backups that were outdated or incomplete. Data loss and difficult recovery were associated with substantial stress.

Source: Wunder et al., CHI 2025.
https://doi.org/10.1145/3706598.3714202

### Design consequence

Recovery cannot be a hidden utility reached only after failure.

File Manager should treat reversibility and recovery as part of normal interaction design:

- reversible deletion is visually and semantically distinct from permanent deletion;
- previous versions/history should be available where supported;
- replacement/overwrite flows should expose recovery consequences;
- interrupted copy/move operations need explicit partial-state handling;
- operation results should be verifiable rather than merely assumed; and
- an operation journal/history can become a user-facing trust surface rather than debug-only logging.

InfiltratorFS retained generations and future object-level restore can make this especially strong, but the UI model must also work on ordinary filesystems.

## 6. Layered interfaces can expose depth without overwhelming users

### Designing for Learnability: Improvement Through Layered Interfaces (2024/2025)

This work proposes progressive disclosure through layered interfaces as a way to improve learnability by initially presenting a subset of functionality and revealing greater depth as needed.

Source: Forsey et al.
https://doi.org/10.1177/10648046241273291

### Malleable Overview-Detail Interfaces (CHI 2025)

This research argues that a single fixed overview/detail presentation cannot satisfy all users. It identifies content, composition and layout as dimensions that can be made more malleable and reports diverse customisations and usage patterns in a user study.

Source: Min, Chen, Cao & Xia, CHI 2025.
https://doi.org/10.1145/3706598.3714164

### Meridian: A Design Framework for Malleable Overview-Detail Interfaces (UIST 2025)

Meridian extends this direction into a design framework for building overview-detail interfaces that are more adaptable to differing information needs.

Source: Min & Xia, UIST 2025.
https://doi.org/10.1145/3746059.3747654

### Design consequence

The default surface should remain calm and immediately understandable while power is progressively available.

For a file manager this suggests:

- a clean primary browse surface;
- a detail/inspection surface that can expose progressively richer metadata;
- user-selectable information density;
- contextual actions rather than permanent toolbar accumulation; and
- adaptable overview/detail presentation without turning every window into a configuration exercise.

### Browse-presentation consequence

This direction argues against treating a single directory layout as universally optimal. Different browsing tasks have different information needs, so Files should support a small number of clearly differentiated presentations without fragmenting the underlying interaction model.

The current design brief is therefore task-based:

- **Detail / analytical** for comparing names and metadata across one-item-per-row records;
- **Visual** for recognition through larger icons/thumbnails and spatial grouping; and
- **Compact / dense scan** for exposing many filenames at once with small icons and labels flowing across multiple columns.

These are different compositions, not merely three icon sizes. In particular, Compact should not be implemented as a miniature Visual grid.

The user should be able to switch between presentations cheaply because the useful presentation can change with the current task. Selection, sorting, activation, drag/drop, context actions and filesystem semantics should remain stable across that switch. View presentation is therefore presentation state, not a second application model.

Density should remain independently controllable where useful. Icon or thumbnail size can vary inside Visual; row density or visible columns can vary inside Detail. This prevents a future zoom/density control from being confused with the identity of the presentation itself.

The baseline persistence rule is global remembered presentation across ordinary application restarts. Per-location view memory remains optional and should only be introduced if testing shows that its benefit outweighs surprise and hidden state.

Validation should include task switching, preserved selection, keyboard/focus behaviour, large-directory responsiveness, narrow windows, Day/Night rendering, and consistent operations across presentations. Exact spacing, control placement and density steps remain prototype questions rather than research conclusions.

## 7. Mercedes-Benz design provides a useful quality rubric

Mercedes-Benz's 2026 brand material describes the brand around technical excellence, quality, reliability and innovation. Its current design philosophy describes "Sensual Purity" as the balance of emotion/intellect and sensuality/reduction. Mercedes-Benz's MBUX "Zero Layer" work similarly attempts to reduce menu traversal by surfacing contextually relevant functions at the top level.

Official sources:

- Mercedes-Benz, *100 years of the Mercedes-Benz brand: Tradition, transformation and technological leadership* (23 June 2026).
  https://media.mercedes-benz.com/en/article/601b9810-1229-4809-aa6a-de31e93145ff
- Mercedes-Benz design, *Creating Icons – Realising Dreams* / Sensual Purity design manifesto.
  https://www.mercedes-benz.com/documents/design/concept-cars/vision-iconic/iconic-design-reading-sample.pdf
- Mercedes-Benz, *MBUX reaches a new level — Zero Layer Concept*.
  https://group.mercedes-benz.com/technology/digitalisation/connectivity/mbux-interior-assist.html

### Design consequence

For this project, "Mercedes-Benz excellence" means a measurable quality target rather than visual mimicry:

- safety/trust → operations with clear consequences and recovery;
- quality → rigorous behaviour under difficult real-world conditions;
- comfort → low-friction common workflows;
- intelligence → contextually useful assistance rather than novelty;
- design/reduction → visual calm with deep capability available when required.

## 8. InfiltratorFS changes what is possible

The file manager should inherit the InfiltratorFS design discipline without becoming dependent on InfiltratorFS.

Relevant InfiltratorFS concepts already documented by that project include:

- persistent 128-bit object identity distinct from path;
- retained historical generations;
- copy-on-write publication;
- reflinks/shared extents;
- sparse files;
- integrity metadata;
- explicit capability/failure semantics; and
- platform-neutral semantics behind OS adapters.

The ecosystem document also already states that applications should use generic interfaces where possible, optionally exploit stronger capabilities, and continue to work correctly on other filesystems.

### Design consequence

File Manager should use a capability-provider architecture.

The strongest experience may appear on InfiltratorFS, but the application itself remains a general-purpose file manager.

## 9. Combined research direction

The research does **not** point toward replacing folders with an AI chat box.

It points toward a layered model:

```text
               user intent
       ┌──────────┼──────────┐
       │          │          │
  navigation    search    semantic/context
       │          │          │
       └──────────┼──────────┘
                  ↓
        authoritative file/object model
                  ↓
         deterministic operation engine
                  ↓
       OS + filesystem capability layer
                  ↓
    ordinary FS / InfiltratorFS / remote FS
```

The novel opportunity is not "AI controls the filesystem". It is **a trustworthy traditional filesystem interface augmented by better retrieval, context, identity and recovery**.

## 10. Initial product hypotheses to test

The following are research hypotheses, not yet frozen requirements:

1. **Navigation/search parity:** users should be able to begin with either browsing or search and move between them without losing context.
2. **Explainable related items:** relationship suggestions will be more trustworthy when the UI shows why each item is related.
3. **Object-aware history:** on InfiltratorFS, history attached to persistent object identity should remain understandable after rename/move.
4. **Recovery-visible operations:** showing operation history/reversibility will improve confidence during destructive or large batch actions.
5. **Progressive technical depth:** ordinary users can keep a clean interface while expert users can reveal integrity, identity, sparse/reflink/history and detailed properties without switching products.
6. **Local semantic retrieval:** a local semantic index can materially improve vague re-finding tasks without becoming required for normal file access.
7. **Contextual zero-layer actions:** a small number of highly relevant actions can reduce menu traversal without hiding the complete action set.
8. **Derived workspaces:** activity/project working sets can reduce re-finding effort without physically reorganising files.
9. **Task-based browse presentation:** fast switching among Detail, Visual and Compact presentations should reduce friction when a browsing task changes, provided all three remain views over the same selection and file-operation model.

## 11. Research still required before implementation architecture is frozen

- compare the strongest current file managers as evidence: Explorer, Finder, Dolphin, Nemo, Directory Opus, Total Commander/Double Commander and other relevant systems;
- benchmark local embedding/index approaches on realistic personal collections;
- evaluate privacy and resource cost of semantic indexing;
- design identity fallbacks for filesystems without persistent object IDs;
- define operation-journal and undo semantics across local, removable and network filesystems;
- test very large directories and metadata-heavy workloads;
- test history UX for object-versus-path identity;
- test novice, conventional navigation-heavy and hyper-searcher workflows separately;
- investigate accessibility and keyboard-first interaction from the beginning;
- compare Detail, Visual and Compact browse presentations using realistic folders and tasks; and
- prototype more than one visual/navigation concept before selecting the production UI.

## Research discipline

No cited paper or competitor becomes an authority over the product. The project should record what problem a source exposes, what mechanism appears useful, what trade-off it introduces, and how an Infiltrator implementation would be validated.
