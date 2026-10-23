// SPDX-License-Identifier: GPL-3.0-or-later
#include "batch_item_operation_controller.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace infiltrator::files {

namespace {

struct BatchTaskData {
    int kind{0};
    int policy{0};
    std::vector<std::string> sources;
    std::string destination_parent;
    std::shared_ptr<TransferControl> control;
};

const char *kind_verb(const BatchTransferKind kind) noexcept
{
    return kind == BatchTransferKind::Copy ? "Copy" : "Move";
}

} // namespace

BatchItemOperationController::BatchItemOperationController(
    GtkWindow *window,
    GtkDirectoryList *directory_list,
    GtkMultiSelection *selection,
    GtkWidget *status_label)
    : mutation_guard_(window, directory_list),
      progress_view_(window, mutation_guard_, status_label, "Batch operation progress"), window_(window), directory_list_(directory_list), selection_(selection),
      status_label_(status_label)
{
    if (window_ == nullptr || directory_list_ == nullptr || selection_ == nullptr) {
        return;
    }

    g_object_ref(directory_list_);
    g_object_ref(selection_);

    GtkWidget *titlebar = gtk_window_get_titlebar(window_);
    if (GTK_IS_HEADER_BAR(titlebar)) {
        menu_button_ = gtk_menu_button_new();
        gtk_menu_button_set_child(GTK_MENU_BUTTON(menu_button_), gtk_label_new("Selected"));
        gtk_widget_set_tooltip_text(menu_button_, "Actions for multiple selected items");

        popover_ = gtk_popover_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_margin_start(box, 8);
        gtk_widget_set_margin_end(box, 8);
        gtk_widget_set_margin_top(box, 8);
        gtk_widget_set_margin_bottom(box, 8);

        copy_button_ = gtk_button_new_with_label("Copy selected to…");
        move_button_ = gtk_button_new_with_label("Move selected to…");
        for (GtkWidget *button : {copy_button_, move_button_}) {
            gtk_widget_add_css_class(button, "flat");
            gtk_widget_set_halign(button, GTK_ALIGN_FILL);
            gtk_box_append(GTK_BOX(box), button);
        }
        g_signal_connect(copy_button_, "clicked",
                         G_CALLBACK(&BatchItemOperationController::on_copy_clicked), this);
        g_signal_connect(move_button_, "clicked",
                         G_CALLBACK(&BatchItemOperationController::on_move_clicked), this);
        gtk_popover_set_child(GTK_POPOVER(popover_), box);
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu_button_), popover_);
        gtk_header_bar_pack_end(GTK_HEADER_BAR(titlebar), menu_button_);

    }

    selection_handler_ = g_signal_connect(selection_, "selection-changed",
                                          G_CALLBACK(&BatchItemOperationController::on_selection_changed),
                                          this);
    location_handler_ = g_signal_connect(directory_list_, "notify::file",
                                         G_CALLBACK(&BatchItemOperationController::on_location_changed),
                                         this);

    update_action_state();
    g_object_weak_ref(G_OBJECT(window_), &BatchItemOperationController::on_window_finalized, this);
}

BatchItemOperationController::~BatchItemOperationController()
{
    if (current_control_) {
        current_control_->request_cancel();
    }
    if (directory_list_ != nullptr) {
        if (location_handler_ != 0U) {
            g_signal_handler_disconnect(directory_list_, location_handler_);
        }
        g_object_unref(directory_list_);
        directory_list_ = nullptr;
    }
    if (selection_ != nullptr) {
        if (selection_handler_ != 0U) {
            g_signal_handler_disconnect(selection_, selection_handler_);
        }
        g_object_unref(selection_);
        selection_ = nullptr;
    }
}

void BatchItemOperationController::on_window_finalized(gpointer user_data,
                                                       GObject *where_object_was)
{
    (void)where_object_was;
    auto *self = static_cast<BatchItemOperationController *>(user_data);
    self->window_ = nullptr;
    delete self;
}

void BatchItemOperationController::on_copy_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    auto *self = static_cast<BatchItemOperationController *>(user_data);
    self->close_menu();
    self->choose_destination(BatchTransferKind::Copy);
}

void BatchItemOperationController::on_move_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    auto *self = static_cast<BatchItemOperationController *>(user_data);
    self->close_menu();
    self->choose_destination(BatchTransferKind::Move);
}

void BatchItemOperationController::on_selection_changed(GtkSelectionModel *model,
                                                        const guint position,
                                                        const guint n_items,
                                                        gpointer user_data)
{
    (void)model;
    (void)position;
    (void)n_items;
    static_cast<BatchItemOperationController *>(user_data)->update_action_state();
}

void BatchItemOperationController::on_location_changed(GObject *object,
                                                       GParamSpec *pspec,
                                                       gpointer user_data)
{
    (void)object;
    (void)pspec;
    auto *self = static_cast<BatchItemOperationController *>(user_data);
    self->chooser_sources_.clear();
    self->pending_sources_.clear();
    self->update_action_state();
}

void BatchItemOperationController::choose_destination(const BatchTransferKind kind)
{
    if (busy_ || chooser_busy_ || window_ == nullptr) {
        return;
    }

    std::vector<std::string> sources = selected_sources();
    if (sources.size() < 2U) {
        return;
    }

    chooser_busy_ = true;
    chooser_kind_ = kind;
    chooser_sources_ = std::move(sources);
    update_action_state();

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, kind == BatchTransferKind::Copy
                                         ? "Copy selected items to"
                                         : "Move selected items to");
    if (GFile *current = gtk_directory_list_get_file(directory_list_); current != nullptr) {
        gtk_file_dialog_set_initial_folder(dialog, current);
    }

    g_object_ref(window_);
    gtk_file_dialog_select_folder(dialog,
                                  window_,
                                  nullptr,
                                  &BatchItemOperationController::on_destination_chosen,
                                  this);
    g_object_unref(dialog);
}

void BatchItemOperationController::on_destination_chosen(GObject *source_object,
                                                         GAsyncResult *result,
                                                         gpointer user_data)
{
    auto *self = static_cast<BatchItemOperationController *>(user_data);
    GtkWindow *held_window = self->window_;

    GError *error = nullptr;
    GFile *folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source_object), result, &error);
    if (!self->mutation_guard_.alive()) {
        g_clear_error(&error);
        g_clear_object(&folder);
        g_object_unref(held_window);
        return;
    }
    self->chooser_busy_ = false;

    if (error != nullptr) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            self->show_alert("Choose destination", error->message);
        }
        g_error_free(error);
        self->chooser_sources_.clear();
        self->update_action_state();
        g_object_unref(held_window);
        return;
    }
    if (folder == nullptr || !g_file_is_native(folder)) {
        if (folder != nullptr) {
            g_object_unref(folder);
        }
        self->show_alert("Choose destination", "Batch copy and move currently require a local destination.");
        self->chooser_sources_.clear();
        self->update_action_state();
        g_object_unref(held_window);
        return;
    }

    char *destination_path = g_file_get_path(folder);
    g_object_unref(folder);
    if (destination_path == nullptr) {
        self->show_alert("Choose destination", "The destination folder could not be resolved.");
        self->chooser_sources_.clear();
        self->update_action_state();
        g_object_unref(held_window);
        return;
    }

    std::vector<std::string> sources = std::move(self->chooser_sources_);
    self->chooser_sources_.clear();
    const std::string destination_parent = destination_path;
    g_free(destination_path);

    self->start_operation(self->chooser_kind_, sources, destination_parent, BatchConflictPolicy::Fail);
    g_object_unref(held_window);
}

void BatchItemOperationController::prompt_conflict(
    const BatchTransferKind kind,
    const std::vector<std::string> &sources,
    const std::string &destination_parent,
    const std::string &detail)
{
    if (window_ == nullptr || chooser_busy_ || sources.empty() || destination_parent.empty()) {
        return;
    }

    chooser_busy_ = true;
    pending_kind_ = kind;
    pending_sources_ = sources;
    pending_destination_parent_ = destination_parent;
    update_action_state();

    GtkAlertDialog *dialog = gtk_alert_dialog_new("Some selected items already exist there");
    const std::string explanation = detail +
        " Choose one policy for conflicting items in this batch. Non-conflicting items keep their original names.";
    gtk_alert_dialog_set_detail(dialog, explanation.c_str());
    const char *buttons[] = {"Cancel", "Skip Conflicts", "Keep Both", "Replace Conflicts", nullptr};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 2);

    g_object_ref(window_);
    gtk_alert_dialog_choose(dialog,
                            window_,
                            nullptr,
                            &BatchItemOperationController::on_conflict_chosen,
                            this);
    g_object_unref(dialog);
}

void BatchItemOperationController::on_conflict_chosen(GObject *source_object,
                                                      GAsyncResult *result,
                                                      gpointer user_data)
{
    auto *self = static_cast<BatchItemOperationController *>(user_data);
    GtkWindow *held_window = self->window_;

    GError *error = nullptr;
    const int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source_object), result, &error);
    if (!self->mutation_guard_.alive()) {
        g_clear_error(&error);
        g_object_unref(held_window);
        return;
    }
    self->chooser_busy_ = false;

    const BatchTransferKind kind = self->pending_kind_;
    std::vector<std::string> sources = std::move(self->pending_sources_);
    const std::string destination_parent = self->pending_destination_parent_;
    self->pending_sources_.clear();
    self->pending_destination_parent_.clear();

    if (error != nullptr) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            self->show_alert("Resolve batch conflicts", error->message);
        }
        g_error_free(error);
        self->update_action_state();
        g_object_unref(held_window);
        return;
    }

    if (choice == 1) {
        self->start_operation(kind, sources, destination_parent, BatchConflictPolicy::Skip);
    } else if (choice == 2) {
        self->start_operation(kind, sources, destination_parent, BatchConflictPolicy::KeepBoth);
    } else if (choice == 3) {
        self->start_operation(kind, sources, destination_parent, BatchConflictPolicy::Replace);
    } else {
        self->update_action_state();
    }
    g_object_unref(held_window);
}

void BatchItemOperationController::start_operation(
    const BatchTransferKind kind,
    const std::vector<std::string> &sources,
    const std::string &destination_parent,
    const BatchConflictPolicy policy)
{
    if (busy_ || sources.size() < 2U || destination_parent.empty() || window_ == nullptr ||
        !mutation_guard_.begin()) {
        return;
    }

    current_control_ = std::make_shared<TransferControl>();
    auto *data = new BatchTaskData{
        static_cast<int>(kind),
        static_cast<int>(policy),
        sources,
        destination_parent,
        current_control_,
    };

    set_busy(true);
    if (status_label_ != nullptr) {
        const std::string status = std::string(kind_verb(kind) == std::string("Copy") ? "Copying " : "Moving ") +
                                   std::to_string(sources.size()) + " selected items…";
        gtk_label_set_text(GTK_LABEL(status_label_), status.c_str());
    }

    GTask *task = g_task_new(G_OBJECT(window_), nullptr,
                             &BatchItemOperationController::on_operation_finished, this);
    g_task_set_task_data(task, data, [](gpointer value) {
        delete static_cast<BatchTaskData *>(value);
    });
    g_task_run_in_thread(task, &BatchItemOperationController::on_operation_thread);
    g_object_unref(task);
}

void BatchItemOperationController::on_operation_thread(GTask *task,
                                                       gpointer source_object,
                                                       gpointer task_data,
                                                       GCancellable *cancellable)
{
    (void)source_object;
    (void)cancellable;
    const auto *data = static_cast<const BatchTaskData *>(task_data);

    std::vector<std::filesystem::path> sources;
    sources.reserve(data->sources.size());
    for (const auto &source : data->sources) {
        sources.emplace_back(source);
    }

    BatchTransferEngine engine;
    auto *operation = new OperationResult(engine.execute(
        sources,
        data->destination_parent,
        static_cast<BatchTransferKind>(data->kind),
        static_cast<BatchConflictPolicy>(data->policy),
        data->control.get()));
    g_task_return_pointer(task, operation, [](gpointer value) {
        delete static_cast<OperationResult *>(value);
    });
}

void BatchItemOperationController::on_operation_finished(GObject *source_object,
                                                         GAsyncResult *result,
                                                         gpointer user_data)
{
    (void)source_object;
    auto *self = static_cast<BatchItemOperationController *>(user_data);
    GTask *task = G_TASK(result);
    const auto *data = static_cast<const BatchTaskData *>(g_task_get_task_data(task));

    GError *error = nullptr;
    std::unique_ptr<OperationResult> operation(
        static_cast<OperationResult *>(g_task_propagate_pointer(task, &error)));
    self->progress_view_.finish();
    if (!self->mutation_guard_.finish()) {
        g_clear_error(&error);
        return;
    }
    self->set_busy(false);
    self->current_control_.reset();

    if (error != nullptr) {
        if (self->status_label_ != nullptr) {
            gtk_label_set_text(GTK_LABEL(self->status_label_), error->message);
        }
        self->show_alert("Batch file operation", error->message);
        g_error_free(error);
        return;
    }
    if (!operation || data == nullptr) {
        self->show_alert("Batch file operation", "The batch operation did not return a result.");
        return;
    }

    const auto kind = static_cast<BatchTransferKind>(data->kind);
    const auto policy = static_cast<BatchConflictPolicy>(data->policy);
    if (operation->status == OperationStatus::DestinationConflict &&
        policy == BatchConflictPolicy::Fail) {
        if (self->status_label_ != nullptr) {
            gtk_label_set_text(GTK_LABEL(self->status_label_), operation->message.c_str());
        }
        self->prompt_conflict(kind, data->sources, data->destination_parent, operation->message);
        return;
    }

    if (self->status_label_ != nullptr) {
        gtk_label_set_text(GTK_LABEL(self->status_label_), operation->message.c_str());
    }
    if (!operation->ok() && operation->status != OperationStatus::Cancelled) {
        self->show_alert(operation->status == OperationStatus::VerificationFailure
                             ? "Batch completed with a verification problem"
                             : operation->status == OperationStatus::UnsupportedCapability
                                   ? "Batch operation not supported safely"
                             : "Batch file operation failed",
                         operation->message);
    }
}

void BatchItemOperationController::set_busy(const bool busy)
{
    if (!mutation_guard_.alive()) return;
    busy_ = busy;
    if (busy) progress_view_.start(current_control_);
    else progress_view_.finish();
    update_action_state();
}

void BatchItemOperationController::update_action_state()
{
    if (!mutation_guard_.alive()) return;
    if (menu_button_ == nullptr) {
        return;
    }
    GFile *parent = directory_list_ != nullptr
        ? gtk_directory_list_get_file(directory_list_) : nullptr;
    GtkBitset *selected = selection_ != nullptr
        ? gtk_selection_model_get_selection(GTK_SELECTION_MODEL(selection_)) : nullptr;
    const bool enabled = !busy_ && !chooser_busy_ && parent != nullptr &&
        g_file_is_native(parent) && selected != nullptr && gtk_bitset_get_size(selected) > 1U;
    if (selected != nullptr) {
        gtk_bitset_unref(selected);
    }
    gtk_widget_set_sensitive(menu_button_, enabled);
    gtk_widget_set_visible(menu_button_, enabled);
}

std::vector<std::string> BatchItemOperationController::selected_sources() const
{
    std::vector<std::string> sources;
    if (directory_list_ == nullptr || selection_ == nullptr) {
        return sources;
    }

    GFile *parent = gtk_directory_list_get_file(directory_list_);
    if (parent == nullptr || !g_file_is_native(parent)) {
        return sources;
    }

    // Selection positions belong to the presentation model. Once Detail sorting
    // is active, indexing the raw GtkDirectoryList with those positions could
    // operate on the wrong files, so selected items must come from that model.
    GListModel *model = gtk_multi_selection_get_model(selection_);
    if (model == nullptr) {
        return sources;
    }
    GtkBitset *selected = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(selection_));
    GtkBitsetIter iterator;
    guint index = 0U;
    for (gboolean valid = gtk_bitset_iter_init_first(&iterator, selected, &index);
         valid; valid = gtk_bitset_iter_next(&iterator, &index)) {
        GFileInfo *info = G_FILE_INFO(g_list_model_get_item(model, index));
        if (info == nullptr) {
            continue;
        }
        const char *name = g_file_info_get_name(info);
        if (name != nullptr && name[0] != '\0') {
            GFile *child = g_file_get_child(parent, name);
            char *path = g_file_get_path(child);
            if (path != nullptr) {
                sources.emplace_back(path);
                g_free(path);
            }
            g_object_unref(child);
        }
        g_object_unref(info);
    }
    gtk_bitset_unref(selected);
    return sources;
}

void BatchItemOperationController::close_menu()
{
    if (!mutation_guard_.alive()) return;
    if (popover_ != nullptr) {
        gtk_popover_popdown(GTK_POPOVER(popover_));
    }
}

void BatchItemOperationController::show_alert(const char *title, const std::string &detail) const
{
    if (window_ == nullptr) {
        return;
    }
    GtkAlertDialog *alert = gtk_alert_dialog_new("%s", title);
    gtk_alert_dialog_set_detail(alert, detail.c_str());
    gtk_alert_dialog_show(alert, window_);
    g_object_unref(alert);
}

} // namespace infiltrator::files
