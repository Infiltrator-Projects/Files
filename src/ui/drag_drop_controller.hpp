// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <gtk/gtk.h>
#include <string>
#include <vector>

namespace infiltrator::files {

// Transport only: the clipboard controller owns the qualified transfer engine.
class DragDropController final {
public:
    DragDropController(GtkWindow *window, GtkDirectoryList *directory,
                       GtkMultiSelection *selection, GtkWidget *stack, GtkWidget *sidebar);
    ~DragDropController();
private:
    static void on_window_finalized(gpointer, GObject *);
    static GdkContentProvider *on_prepare(GtkDragSource *, double, double, gpointer);
    static gboolean on_drop(GtkDropTarget *, const GValue *, double, double, gpointer);
    static GdkDragAction on_motion(GtkDropTarget *, double, double, gpointer);
    void register_view(GtkWidget *view);
    std::vector<std::string> selected_paths(GtkWidget *view, double x, double y);
    GFile *target_folder(GtkWidget *target, double x, double y) const;
    GtkWindow *window_{nullptr};
    GtkDirectoryList *directory_{nullptr};
    GtkMultiSelection *selection_{nullptr};
    // Owned references keep the secondary model valid until the window closes.
    // The actual pane widget remains weak and is window-owned.
    GtkDirectoryList *secondary_directory_{nullptr};
    GtkMultiSelection *secondary_selection_{nullptr};
    GtkWidget *secondary_view_{nullptr};
    GtkWidget *stack_{nullptr};
    GtkWidget *sidebar_{nullptr};
};

} // namespace infiltrator::files
