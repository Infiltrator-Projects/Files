// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <gtk/gtk.h>

namespace infiltrator::files {
// Only the selected mounted partition's root is eligible; discovery never
// changes the volume. The privileged host verifies it again before writing.
[[nodiscard]] bool volume_format_available(GFile *folder);
void show_volume_format_dialog(GtkWindow *parent, GFile *folder);
} // namespace infiltrator::files
