// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <gtk/gtk.h>
namespace infiltrator::files {
// Opens a read-only, cancellable search against a captured GIO directory.
// Results reveal source objects through the owning FileManagerWindow.
void show_recursive_search(GtkWindow *owner, GFile *location);
} // namespace infiltrator::files
