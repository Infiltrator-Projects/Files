// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <gtk/gtk.h>

namespace infiltrator::files {

// Read-only inspection owns its own cancellable workers and dialog lifetime.
// No inspection result is ever authority for a filesystem mutation.
void show_file_inspection(GtkWindow *parent, GFile *file, GFileInfo *known);
void compare_file_hashes(GtkWindow *parent, GFile *first, GFile *second);
void open_terminal_in_directory(GtkWindow *parent, GFile *directory);

} // namespace infiltrator::files
