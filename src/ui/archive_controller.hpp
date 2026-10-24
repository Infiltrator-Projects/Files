// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "directory_mutation_guard.hpp"
#include <gtk/gtk.h>
#include <string>

namespace infiltrator::files {

// Owns archive interaction and journalling, never archive bytes or publication.
class ArchiveController final {
public:
    ArchiveController(GtkWindow *window, GtkDirectoryList *directory,
                      GtkSingleSelection *selection, GtkWidget *status);
    ~ArchiveController();
    void extract_selected();
    void create_selected();
    ArchiveController(const ArchiveController &) = delete;
    ArchiveController &operator=(const ArchiveController &) = delete;
private:
    void start_selected(bool create);
    static void on_window_finalized(gpointer, GObject *);
    static void on_window_destroyed(GtkWidget *, gpointer);
    static void on_thread(GTask *, gpointer, gpointer, GCancellable *);
    static void on_finished(GObject *, GAsyncResult *, gpointer);
    DirectoryMutationGuard guard_;
    GtkWindow *window_{nullptr};
    GtkDirectoryList *directory_{nullptr};
    GtkSingleSelection *selection_{nullptr};
    GtkWidget *status_{nullptr};
    GCancellable *cancellable_{nullptr};
    bool busy_{false};
};

} // namespace infiltrator::files
