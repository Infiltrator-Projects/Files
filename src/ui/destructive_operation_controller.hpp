// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "directory_mutation_guard.hpp"
#include "transfer_progress_view.hpp"

#include <gtk/gtk.h>

#include <filesystem>
#include <string>

namespace infiltrator::files {

class DestructiveOperationController final {
public:
    DestructiveOperationController(GtkWindow *window,
                                   GtkDirectoryList *directory_list,
                                   GtkSingleSelection *selection,
                                   GtkWidget *status_label,
                                   GtkMultiSelection *multi_selection = nullptr);
    ~DestructiveOperationController();

    DestructiveOperationController(const DestructiveOperationController &) = delete;
    DestructiveOperationController &operator=(const DestructiveOperationController &) = delete;

private:
    enum class Kind {
        Trash,
        Restore,
        RestoreReplace,
        DeletePath,
        DeleteUri,
        BatchTrash,
        BatchDelete,
    };

    static void on_window_finalized(gpointer user_data, GObject *where_object_was);
    static void on_trash_clicked(GtkButton *button, gpointer user_data);
    static void on_restore_clicked(GtkButton *button, gpointer user_data);
    static void on_delete_clicked(GtkButton *button, gpointer user_data);
    static gboolean on_key_pressed(GtkEventControllerKey *controller,
                                   guint keyval,
                                   guint keycode,
                                   GdkModifierType state,
                                   gpointer user_data);
    static void on_selection_changed(GObject *object, GParamSpec *pspec, gpointer user_data);
    static void on_location_changed(GObject *object, GParamSpec *pspec, gpointer user_data);
    static void on_delete_confirmed(GObject *source_object,
                                    GAsyncResult *result,
                                    gpointer user_data);
    static void on_restore_replace_chosen(GObject *source_object,
                                          GAsyncResult *result,
                                          gpointer user_data);
    static void on_operation_thread(GTask *task,
                                    gpointer source_object,
                                    gpointer task_data,
                                    GCancellable *cancellable);
    static void on_operation_finished(GObject *source_object,
                                      GAsyncResult *result,
                                      gpointer user_data);

    void move_selected_to_trash();
    void restore_selected();
    void confirm_permanent_delete();
    void prompt_restore_replace(const std::string &trash_uri,
                                const std::string &original_path,
                                const std::string &detail);
    void start_operation(Kind kind,
                         const std::string &source_path,
                         const std::string &trash_uri,
                         const std::string &original_path);
    void start_batch(Kind kind, const std::vector<std::string> &sources);
    [[nodiscard]] std::vector<std::string> selected_native_sources() const;
    void update_action_state();
    void close_menu();
    void show_alert(const char *title, const std::string &detail) const;
    [[nodiscard]] bool current_location_is_trash() const;
    [[nodiscard]] bool selected_local_source(std::string &path, std::string &name) const;
    [[nodiscard]] bool selected_trash_source(std::string &uri,
                                             std::string &original_path,
                                             std::string &name,
                                             bool require_original) const;

    DirectoryMutationGuard mutation_guard_;
    TransferProgressView progress_view_;
    GtkWindow *window_{nullptr};
    GtkDirectoryList *directory_list_{nullptr};
    GtkSingleSelection *selection_{nullptr};
    GtkMultiSelection *multi_selection_{nullptr};
    gulong multi_handler_{0U};
    GtkWidget *status_label_{nullptr};
    GtkWidget *menu_button_{nullptr};
    GtkWidget *popover_{nullptr};
    GtkWidget *trash_button_{nullptr};
    GtkWidget *restore_button_{nullptr};
    GtkWidget *delete_button_{nullptr};
    gulong selection_handler_{0U};
    gulong location_handler_{0U};
    bool busy_{false};
    bool dialog_busy_{false};
    Kind pending_kind_{Kind::DeletePath};
    std::string pending_source_path_;
    std::string pending_trash_uri_;
    std::string pending_original_path_;
    std::vector<std::string> pending_sources_;
};

} // namespace infiltrator::files
