// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <gtk/gtk.h>

namespace infiltrator::files {

// Opens a nonmutating, bounded preview over a snapshot of the visible file
// ordering. Each active preview owns one cancellable GTask, and GTK observes
// only the latest generation while disposed windows cannot be dereferenced.
void show_quick_preview(GtkWindow *owner, GtkDirectoryList *listing,
                        GtkMultiSelection *visible, GtkSingleSelection *primary);
} // namespace infiltrator::files
