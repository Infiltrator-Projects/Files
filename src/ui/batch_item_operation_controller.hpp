// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "directory_mutation_guard.hpp"

#include "../core/batch_transfer_engine.hpp"

#include <gtk/gtk.h>

#include <memory>
#include <string>
#include <vector>

namespace infiltrator::files {

class BatchItemOperationController final {
public:
    BatchItemOperationController(GtkWindow *window,
                                 GtkDirectoryList *directory_list,
                                 GtkMultiSelection *selection,
                                 GtkWidget *status_label);
    ~BatchItemOperationController();

    BatchItemOperationController(const BatchItemOperationController &) = delete;
    BatchItemOperationController &operator=(const BatchItemOperationController &) = delete;

private:
    static void on_window_finalized(gpointer user_data, GObject *where_object_was);
    static void on_copy_clicked(GtkButton *button, gpointer user_data);
    static void on_move_clicked(GtkButton *button, gpointer user_data);
    static void on_cancel_clicked(GtkButton *button, gpointer user_data);
    static void on_selection_changed(GtkSelectionModel *model,
                                     guint position,
                                     guint n_items,
                                     gpointer user_data);
    static void on_location_changed(GObject *object, GParamSpec *pspec, gpointer user_data);
    static void on_destination_chosen(GObject *source_object,
                                      GAsyncResult *result,
                                      gpointer user_data);
    static void on_conflict_chosen(GObject *source_object,
                                   GAsyncResult *result,
                                   gpointer user_data);
    static void on_operation_thread(GTask *task,
                                    gpointer source_object,
                                    gpointer task_data,
                                    GCancellable *cancellable);
    static void on_operation_finished(GObject *source_object,
                                      GAsyncResult *result,
                                      gpointer user_data);
    static gboolean on_progress_tick(gpointer user_data);

    void choose_destination(BatchTransferKind kind);
    void prompt_conflict(BatchTransferKind kind,
                         const std::vector<std::string> &sources,
                         const std::string &destination_parent,
                         const std::string &detail);
    void start_operation(BatchTransferKind kind,
                         const std::vector<std::string> &sources,
                         const std::string &destination_parent,
                         BatchConflictPolicy policy);
    void set_busy(bool busy);
    void update_action_state();
    void update_progress();
    void close_menu();
    void show_alert(const char *title, const std::string &detail) const;
    [[nodiscard]] std::vector<std::string> selected_sources() const;

    DirectoryMutationGuard mutation_guard_;
    GtkWindow *window_{nullptr};
    GtkDirectoryList *directory_list_{nullptr};
    GtkMultiSelection *selection_{nullptr};
    GtkWidget *status_label_{nullptr};
    GtkWidget *menu_button_{nullptr};
    GtkWidget *popover_{nullptr};
    GtkWidget *copy_button_{nullptr};
    GtkWidget *move_button_{nullptr};
    GtkWidget *operation_box_{nullptr};
    GtkWidget *progress_bar_{nullptr};
    GtkWidget *cancel_button_{nullptr};
    gulong selection_handler_{0U};
    gulong location_handler_{0U};
    guint progress_source_id_{0U};
    bool busy_{false};
    bool chooser_busy_{false};
    BatchTransferKind chooser_kind_{BatchTransferKind::Copy};
    BatchTransferKind pending_kind_{BatchTransferKind::Copy};
    std::vector<std::string> chooser_sources_;
    std::vector<std::string> pending_sources_;
    std::string pending_destination_parent_;
    std::shared_ptr<TransferControl> current_control_;
};

} // namespace infiltrator::files
