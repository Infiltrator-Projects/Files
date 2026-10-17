# Files 0.1.41 — Details density and header consistency

A targeted presentation correction based on Linux Mint screenshots of Files
0.1.39. Files 0.1.40 calendar resource integration is preserved unchanged.

- Stop GTK's native ColumnView row and cell insets from silently adding
  excessive height beyond the application's own layout. Use 20px detail
  icons with small, explicit cell margins.
- Check the *rendered* GTK Details-row allocation in the Xvfb visual
  regression test; fail if the visible row height is above 38px at test DPI.
  This targets the previous ~45px layout, not just the CSS min-height.
- Increase filename and metadata legibility while retaining a clear priority
  for names and without cluttering the directory surface.
- Remove the host GTK `linked` styling from the view-mode toggle group.
  Explicitly style Day/Night normal, hover, checked and checked-hover controls
  with Common-owned neutral surfaces, borders and text colours, fixing dark
  unselected controls on a light Day footer.
- Unify the titlebar and tab rail on Common's titlebar surface; use a
  subtle separator and retain one cyan active-tab edge.
- Preserve independent Day/Night per-window CSS provider scoping and active
  view selection, while retaining keyboard and accessible tooltips.
- Keep the native file operation, clipboard, archive, journal, calendar,
  provider and navigation engines unchanged.

All colour values and typography remain referenced from Libraries/Common's
semantic design ABI. Files does not define or duplicate a new palette.

Automated checks verify layout under hosted GTK/Xvfb; actual desktop
rendering and perceived quality must still be verified in Linux Mint.
