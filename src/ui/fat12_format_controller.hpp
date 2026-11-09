// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <gtk/gtk.h>

namespace infiltrator::files {
// A native folder resolves to its enclosing removable partition, never to
// its own directory bytes. No formatting occurs during discovery.
[[nodiscard]] bool fat12_format_available(GFile *folder);
void show_fat12_format_dialog(GtkWindow *parent, GFile *folder);
} // namespace infiltrator::files
