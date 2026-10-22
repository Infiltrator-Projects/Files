// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "directory_mutation_guard.hpp"

#include <gtk/gtk.h>

#include <filesystem>
#include <memory>
#include <string>

namespace infiltrator::files {

class TransferControl;

class ItemOperationController final {
public:
    ItemOperationController(GtkWindow *window,
                            GtkDirectoryList *directory_list,
                            GtkSingleSelection *selection,
                            GtkWidget *status_label);
    ~ItemOperationController();
    void rename_selected_to(const std::string &new_name);

    ItemOperationController(const ItemOperationController &) = delete;
    ItemOperationController &operator=(const ItemOperationController &) = delete;

private:
    enum class Kind {
        Rename,
        Copy,
        Move,
        KeepBothCopy,
        KeepBothMove,
        ReplaceCopy,
        ReplaceMove,
    };

    static void on_window_finalized(gpointer user_data, GObject *where_object_was);
    static void on_rename_clicked(GtkButton *button, gpointer user_data);
    static void on_copy_clicked(GtkButton *button, gpointer user_data);
    static void on_move_clicked(GtkButton *button, gpointer user_data);
    static void on_cancel_operation_clicked(GtkButton *button, gpointer user_data);
    static gboolean on_key_pressed(GtkEventControllerKey *controller,
                                   guint keyval,
                                   guint keycode,
                                   GdkModifierType state,
                                   gpointer user_data);
    static void on_selection_changed(GObject *object, GParamSpec *pspec, gpointer user_data);
    static void on_location_changed(GObject *object, GParamSpec *pspec, gpointer user_data);
    static void on_items_changed(GListModel *model,
                                 guint position,
                                 guint removed,
                                 guint added,
                                 gpointer user_data);
    static void on_rename_dialog_destroy(GtkWidget *widget, gpointer user_data);
    static void on_rename_cancel(GtkButton *button, gpointer user_data);
    static void on_rename_submit(GtkButton *button, gpointer user_data);
    static void on_rename_entry_activate(GtkEntry *entry, gpointer user_data);
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

    void show_rename_dialog();
    void submit_rename();
    void choose_destination(Kind kind);
    void prompt_conflict(Kind completed_kind,
                         const std::string &source_path,
                         const std::string &destination_parent,
                         const std::string &detail);
    void start_operation(Kind kind,
                         const std::string &source_path,
                         const std::string &destination_parent,
                         const std::string &new_name);
    void set_busy(bool busy);
    void update_action_state();
    void update_progress();
    void select_pending_item();
    void close_menu();
    void show_alert(const char *title, const std::string &detail) const;
    [[nodiscard]] bool selected_source(std::string &path, std::string &name) const;
    [[nodiscard]] bool current_location_is_path(const std::filesystem::path &path) const;
    [[nodiscard]] static const char *kind_name(Kind kind) noexcept;
    [[nodiscard]] static bool kind_is_transfer(Kind kind) noexcept;

    DirectoryMutationGuard mutation_guard_;
    GtkWindow *window_{nullptr};
    GtkDirectoryList *directory_list_{nullptr};
    GtkSingleSelection *selection_{nullptr};
    GtkWidget *status_label_{nullptr};
    GtkWidget *menu_button_{nullptr};
    GtkWidget *popover_{nullptr};
    GtkWidget *rename_button_{nullptr};
    GtkWidget *copy_button_{nullptr};
    GtkWidget *move_button_{nullptr};
    GtkWidget *operation_box_{nullptr};
    GtkWidget *progress_bar_{nullptr};
    GtkWidget *cancel_operation_button_{nullptr};
    GtkWidget *rename_dialog_{nullptr};
    GtkWidget *rename_entry_{nullptr};
    GtkWidget *rename_message_{nullptr};
    gulong selection_handler_{0U};
    gulong location_handler_{0U};
    gulong items_handler_{0U};
    guint progress_source_id_{0U};
    bool busy_{false};
    bool chooser_busy_{false};
    Kind chooser_kind_{Kind::Copy};
    Kind pending_conflict_kind_{Kind::Copy};
    std::shared_ptr<TransferControl> current_control_;
    std::string chooser_source_path_;
    std::string rename_source_path_;
    std::string pending_selection_name_;
    std::string pending_conflict_source_path_;
    std::string pending_conflict_destination_parent_;
};

} // namespace infiltrator::files
