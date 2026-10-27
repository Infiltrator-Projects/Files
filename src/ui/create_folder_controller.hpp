// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "directory_mutation_guard.hpp"

#include <gtk/gtk.h>

#include <string>

namespace infiltrator::files {

class CreateFolderController final {
public:
    CreateFolderController(GtkWindow *window,
                           GtkDirectoryList *directory_list,
                           GtkSingleSelection *selection,
                           GtkWidget *status_label);
    ~CreateFolderController();

    CreateFolderController(const CreateFolderController &) = delete;
    CreateFolderController &operator=(const CreateFolderController &) = delete;

private:
    static void on_window_finalized(gpointer user_data, GObject *where_object_was);
    static void on_new_folder_clicked(GtkButton *button, gpointer user_data);
    static void on_new_file_clicked(GtkButton *button, gpointer user_data);
    static void on_new_sparse_clicked(GtkButton *button, gpointer user_data);
    static gboolean on_key_pressed(GtkEventControllerKey *controller,
                                   guint keyval,
                                   guint keycode,
                                   GdkModifierType state,
                                   gpointer user_data);
    static void on_location_changed(GObject *object, GParamSpec *pspec, gpointer user_data);
    static void on_items_changed(GListModel *model,
                                 guint position,
                                 guint removed,
                                 guint added,
                                 gpointer user_data);
    static void on_dialog_destroy(GtkWidget *widget, gpointer user_data);
    static void on_dialog_cancel(GtkButton *button, gpointer user_data);
    static void on_dialog_create(GtkButton *button, gpointer user_data);
    static void on_dialog_entry_activate(GtkEntry *entry, gpointer user_data);
    static void on_create_folder_thread(GTask *task,
                                        gpointer source_object,
                                        gpointer task_data,
                                        GCancellable *cancellable);
    static void on_create_folder_finished(GObject *source_object,
                                          GAsyncResult *result,
                                          gpointer user_data);

    void show_dialog(bool file = false, bool sparse = false);
    void submit_dialog();
    void set_busy(bool busy);
    void update_action_state();
    void select_pending_item();
    void update_status_count();
    void show_alert(const char *title, const std::string &detail) const;
    [[nodiscard]] bool current_location_is(const std::string &uri) const;

    DirectoryMutationGuard mutation_guard_;
    GtkWindow *window_{nullptr};
    GtkDirectoryList *directory_list_{nullptr};
    GtkSingleSelection *selection_{nullptr};
    GtkWidget *status_label_{nullptr};
    GtkWidget *button_{nullptr};
    GtkWidget *new_file_button_{nullptr};
    GtkWidget *sparse_button_{nullptr};
    GtkWidget *sparse_size_{nullptr};
    bool creating_sparse_{false};
    bool creating_file_{false};
    GtkWidget *dialog_{nullptr};
    GtkWidget *entry_{nullptr};
    GtkWidget *dialog_message_{nullptr};
    GtkWidget *inline_rename_checkbox_{nullptr};
    std::string pending_inline_name_;
    gulong location_handler_{0U};
    gulong items_handler_{0U};
    bool busy_{false};
    std::string pending_selection_name_;
};

} // namespace infiltrator::files
