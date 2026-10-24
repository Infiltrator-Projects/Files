// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <gtk/gtk.h>
#include <vector>
namespace infiltrator::files {
// Opens a read-only, cancellable search against a captured GIO directory.
// Results reveal source objects through the owning FileManagerWindow.
void show_recursive_search(GtkWindow *owner, GFile *location);
void show_selected_location_search(GtkWindow *owner,
    const std::vector<GFile *> &directories);
} // namespace infiltrator::files
