# Files 0.1.39 — Common visual system

This release is a **presentation-only** design pass over the working Files
0.1.38 base. Files is not a Defragmenter dashboard: browsing should remain
visually quiet, with one cyan accent, legible type and many neutral surfaces.

## Source of truth and colour rules

All colours below are roles in `InfiltratrThemePalette` from Libraries/Common.
No local palette values, copied Defragmenter RGB constants or product-owned
parallel theme tokens are permitted. Common also owns the MB Corpo typography
families and metrics. The stylesheet remains scoped per native Files window,
so separately configured Day and Night windows are isolated.

| Files surface | Common role | Reason |
|---|---|---|
| Overall native window | `background_rgb` | Shared application's outer colour |
| Browse workspace, icon grid and file rows | Night: `surface_rgb`; Day: `card_rgb` | Dark is not nearly pure black; Day is not blinding white |
| Header/titlebar | `titlebar_rgb` / `title_rgb` | Contrast independent of host GTK theme |
| Sidebar and tab rail | Night: `panel_rgb`; Day: `surface_rgb` | Consistent visual layer adjacent to content |
| Column headers and status strip | `panel_rgb` | Separate controls without extra cards |
| Body text and filenames | `text_rgb` | High readability |
| Metadata and status text | `summary_rgb` | Visible but subordinate |
| Section captions | `kicker_rgb` | Quiet structural labels |
| Hover and active-button fill | `surface_hover_rgb` / `card_hover_rgb` | Neutral feedback |
| Selected rows and places | `selection_background_rgb`, `selection_foreground_rgb` | Semantic selection, not accent-as-background |
| Active tab underline, focus outline and current-path border | `neutral_accent_rgb` | Canonical cyan line-work, not decorative flood |
| Icons associated with selected places | `info_rgb` | Accessible accent text |
| Separators, controls and status lines | `status_border_rgb`, `border_rgb` | Subtle but visible boundaries |
| Inline rename and address entry | `input_rgb`, `text_rgb` | Legible editing |
| Destructive/success/warning controls | Common state roles only when meaningful | Never decorate ordinary file lists with state colours |

## Ten implemented visual refinements

1. **Workspace:** move content and rows off the global near-black
   background role while preserving the Common colour contracts.
2. **Titlebar:** use an explicit title foreground and quiet border; use
   the branded MB Corpo title family rather than ad hoc fonts.
3. **Breadcrumb:** give the current directory a restrained cyan focus cue.
4. **Sidebar:** tighter rows, distinct section captions and a consistent
   selected-place surface/indicator.
5. **Details list:** smaller row padding and 20-pixel glyphs, improving file
   scanning density without removing space for thumbnails.
6. **Text hierarchy:** slightly stronger filenames, secondary metadata from
   Common's summary role and readable column headings.
7. **Selection/focus:** preserve Common selection fill and high-contrast text,
   with one cyan edge/focus marker instead of colouring entire rows cyan.
8. **Tabs:** reduce vertical overhead, clarify the active tab, and retain
   readable close buttons even on inactive tabs.
9. **Status and view control:** quiet footer surface, subtle border and
   legible active-mode control.
10. **Icon/Compact and empty states:** tighten icon composition and harmonise
    no-content, hover, and menu feedback.

## Release discipline

No changes to file operations, journals, provider semantics, chronology,
clipboard actions or preferences schemas. Existing user-selected Day/Night
policy remains unchanged. The theme-presentation test must validate both
themes, live switching and separation of two simultaneously open windows.
The hosted test suite and both DEB/native installer gates must pass before
0.1.39 is published. Screenshots on real Linux Mint remain the final visual
qualification, not something a headless test can prove.
