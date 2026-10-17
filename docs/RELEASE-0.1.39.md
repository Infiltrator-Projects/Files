# Files 0.1.39 — Common palette visual refinement

Files 0.1.39 is an intentional **presentation-only** release.

- Dark directory workspace now uses Common's softer surface role rather than the near-black global page background. Day uses the neutral card role.
- Header, sidebar, tab rail, file list, icon grid and status strip follow a consistent semantic surface hierarchy across Day and Night.
- Readable filename typography takes priority over muted but accessible metadata. The brand title uses Common's MB Corpo branding family.
- Details use a denser scan layout, with reduced row padding, 20 px native icon sizing and clearer column headers.
- Active current directory and active tab share a restrained cyan line accent. Selection remains Common's semantic selection surface and text colours.
- Sidebar spacing, section captions, tabs, inactive close buttons, hover/focus and view-mode controls are refined.
- No RGB palette duplicates, no Defragmenter dashboard cards and no new file-operation features.
- Added actual GTK widget regression assertions for title, sidebar and status text in two independent Day/Night windows, including switching.
- The Files colour-role ledger and ten implementation decisions live in `docs/UI-DESIGN-0.1.39.md`.

File operations, journalling, transport, providers, dates and preferences schemas have not been touched. Headless CI validates semantic colour assignments but screenshots on actual desktop themes are still essential visual confirmation.
