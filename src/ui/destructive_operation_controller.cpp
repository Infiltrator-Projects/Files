// SPDX-License-Identifier: GPL-3.0-or-later
#include "destructive_operation_controller.hpp"

#include "../core/operation_journal.hpp"
#include "../core/recovery_operation_engine.hpp"

#include <memory>
#include <string>

namespace infiltrator::files {

namespace {

struct DestructiveTaskData {
    int kind{0};
    std::string source_path;
    std::string trash_uri;
    std::string original_path;
    std::string journal_id;
};

const char *kind_name(const int kind)
{
    switch (kind) {
    case 0:
        return "trash";
    case 1:
        return "restore";
    case 2:
        return "restore-replace";
    case 3:
        return "delete-path";
    case 4:
        return "delete-uri";
    default:
        return "destructive-operation";
    }
}

const char *busy_text(const int kind)
{
    switch (kind) {
    case 0:
        return "Moving item to Trash…";
    case 1:
    case 2:
        return "Restoring item…";
    case 3:
    case 4:
        return "Permanently deleting item…";
    default:
        return "Working…";
    }
}

} // namespace

DestructiveOperationController::DestructiveOperationController(
    GtkWindow *window,
    GtkDirectoryList *directory_list,
    GtkSingleSelection *selection,
    GtkWidget *status_label)
    : mutation_guard_(window, directory_list), window_(window), directory_list_(directory_list), selection_(selection),
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
        GtkWidget *icon = gtk_image_new_from_icon_name("user-trash-symbolic");
        gtk_menu_button_set_child(GTK_MENU_BUTTON(menu_button_), icon);
        gtk_widget_set_tooltip_text(menu_button_, "Trash, restore and permanent delete");

        popover_ = gtk_popover_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_margin_start(box, 8);
        gtk_widget_set_margin_end(box, 8);
        gtk_widget_set_margin_top(box, 8);
        gtk_widget_set_margin_bottom(box, 8);

        trash_button_ = gtk_button_new_with_label("Move to Trash");
        restore_button_ = gtk_button_new_with_label("Restore");
        delete_button_ = gtk_button_new_with_label("Delete Permanently…");
        for (GtkWidget *button : {trash_button_, restore_button_, delete_button_}) {
            gtk_widget_add_css_class(button, "flat");
            gtk_widget_set_halign(button, GTK_ALIGN_FILL);
            gtk_box_append(GTK_BOX(box), button);
        }
        gtk_widget_add_css_class(delete_button_, "destructive-action");

        g_signal_connect(trash_button_, "clicked",
                         G_CALLBACK(&DestructiveOperationController::on_trash_clicked), this);
        g_signal_connect(restore_button_, "clicked",
                         G_CALLBACK(&DestructiveOperationController::on_restore_clicked), this);
        g_signal_connect(delete_button_, "clicked",
                         G_CALLBACK(&DestructiveOperationController::on_delete_clicked), this);

        gtk_popover_set_child(GTK_POPOVER(popover_), box);
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu_button_), popover_);
        gtk_header_bar_pack_end(GTK_HEADER_BAR(titlebar), menu_button_);
    }

    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed",
                     G_CALLBACK(&DestructiveOperationController::on_key_pressed), this);
    gtk_widget_add_controller(GTK_WIDGET(window_), keys);

    selection_handler_ = g_signal_connect(selection_, "notify::selected",
                                          G_CALLBACK(&DestructiveOperationController::on_selection_changed),
                                          this);
    location_handler_ = g_signal_connect(directory_list_, "notify::file",
                                         G_CALLBACK(&DestructiveOperationController::on_location_changed),
                                         this);

    update_action_state();
    g_object_weak_ref(G_OBJECT(window_), &DestructiveOperationController::on_window_finalized, this);
}

DestructiveOperationController::~DestructiveOperationController()
{
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

void DestructiveOperationController::on_window_finalized(gpointer user_data,
                                                          GObject *where_object_was)
{
    (void)where_object_was;
    auto *self = static_cast<DestructiveOperationController *>(user_data);
    self->window_ = nullptr;
    delete self;
}

void DestructiveOperationController::on_trash_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    auto *self = static_cast<DestructiveOperationController *>(user_data);
    self->close_menu();
    self->move_selected_to_trash();
}

void DestructiveOperationController::on_restore_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    auto *self = static_cast<DestructiveOperationController *>(user_data);
    self->close_menu();
    self->restore_selected();
}

void DestructiveOperationController::on_delete_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    auto *self = static_cast<DestructiveOperationController *>(user_data);
    self->close_menu();
    self->confirm_permanent_delete();
}

gboolean DestructiveOperationController::on_key_pressed(GtkEventControllerKey *controller,
                                                         const guint keyval,
                                                         const guint keycode,
                                                         const GdkModifierType state,
                                                         gpointer user_data)
{
    (void)controller;
    (void)keycode;
    auto *self = static_cast<DestructiveOperationController *>(user_data);
    if (keyval != GDK_KEY_Delete || self->busy_ || self->dialog_busy_) {
        return FALSE;
    }

    if ((state & GDK_SHIFT_MASK) != 0U || self->current_location_is_trash()) {
        self->confirm_permanent_delete();
    } else {
        self->move_selected_to_trash();
    }
    return TRUE;
}

void DestructiveOperationController::on_selection_changed(GObject *object,
                                                          GParamSpec *pspec,
                                                          gpointer user_data)
{
    (void)object;
    (void)pspec;
    static_cast<DestructiveOperationController *>(user_data)->update_action_state();
}

void DestructiveOperationController::on_location_changed(GObject *object,
                                                         GParamSpec *pspec,
                                                         gpointer user_data)
{
    (void)object;
    (void)pspec;
    static_cast<DestructiveOperationController *>(user_data)->update_action_state();
}

void DestructiveOperationController::move_selected_to_trash()
{
    if (busy_ || dialog_busy_ || current_location_is_trash()) {
        return;
    }

    std::string source_path;
    std::string name;
    if (!selected_local_source(source_path, name)) {
        return;
    }
    start_operation(Kind::Trash, source_path, {}, {});
}

void DestructiveOperationController::restore_selected()
{
    if (busy_ || dialog_busy_ || !current_location_is_trash()) {
        return;
    }

    std::string trash_uri;
    std::string original_path;
    std::string name;
    if (!selected_trash_source(trash_uri, original_path, name, true)) {
        show_alert("Restore", "The original location for this Trash item is not available.");
        return;
    }
    start_operation(Kind::Restore, {}, trash_uri, original_path);
}

void DestructiveOperationController::confirm_permanent_delete()
{
    if (busy_ || dialog_busy_ || window_ == nullptr) {
        return;
    }

    std::string name;
    pending_source_path_.clear();
    pending_trash_uri_.clear();
    pending_original_path_.clear();

    if (current_location_is_trash()) {
        if (!selected_trash_source(pending_trash_uri_, pending_original_path_, name, false)) {
            return;
        }
        pending_kind_ = Kind::DeleteUri;
    } else {
        if (!selected_local_source(pending_source_path_, name)) {
            return;
        }
        pending_kind_ = Kind::DeletePath;
    }

    dialog_busy_ = true;
    update_action_state();

    GtkAlertDialog *dialog = gtk_alert_dialog_new("Permanently delete “%s”?", name.c_str());
    gtk_alert_dialog_set_detail(
        dialog,
        "This bypasses recoverable Trash restoration. Files cannot undo this permanent deletion.");
    const char *buttons[] = {"Cancel", "Delete Permanently", nullptr};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 0);

    g_object_ref(window_);
    gtk_alert_dialog_choose(dialog,
                            window_,
                            nullptr,
                            &DestructiveOperationController::on_delete_confirmed,
                            this);
    g_object_unref(dialog);
}

void DestructiveOperationController::on_delete_confirmed(GObject *source_object,
                                                          GAsyncResult *result,
                                                          gpointer user_data)
{
    auto *self = static_cast<DestructiveOperationController *>(user_data);
    GtkWindow *held_window = self->window_;

    GError *error = nullptr;
    const int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source_object), result, &error);
    if (!self->mutation_guard_.alive()) {
        g_clear_error(&error);
        g_object_unref(held_window);
        return;
    }
    self->dialog_busy_ = false;

    const Kind kind = self->pending_kind_;
    const std::string source_path = self->pending_source_path_;
    const std::string trash_uri = self->pending_trash_uri_;
    const std::string original_path = self->pending_original_path_;
    self->pending_source_path_.clear();
    self->pending_trash_uri_.clear();
    self->pending_original_path_.clear();

    if (error != nullptr) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            self->show_alert("Permanent delete", error->message);
        }
        g_error_free(error);
        self->update_action_state();
        g_object_unref(held_window);
        return;
    }

    if (choice == 1) {
        self->start_operation(kind, source_path, trash_uri, original_path);
    } else {
        self->update_action_state();
    }
    g_object_unref(held_window);
}

void DestructiveOperationController::prompt_restore_replace(
    const std::string &trash_uri,
    const std::string &original_path,
    const std::string &detail)
{
    if (window_ == nullptr || dialog_busy_ || trash_uri.empty() || original_path.empty()) {
        return;
    }

    pending_kind_ = Kind::RestoreReplace;
    pending_trash_uri_ = trash_uri;
    pending_original_path_ = original_path;
    dialog_busy_ = true;
    update_action_state();

    GtkAlertDialog *dialog = gtk_alert_dialog_new("Replace the item at the original location?");
    const std::string explanation = detail +
        " Files will stage the existing item and restore it if the Trash restore fails before verification.";
    gtk_alert_dialog_set_detail(dialog, explanation.c_str());
    const char *buttons[] = {"Cancel", "Replace", nullptr};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 1);

    g_object_ref(window_);
    gtk_alert_dialog_choose(dialog,
                            window_,
                            nullptr,
                            &DestructiveOperationController::on_restore_replace_chosen,
                            this);
    g_object_unref(dialog);
}

void DestructiveOperationController::on_restore_replace_chosen(GObject *source_object,
                                                                GAsyncResult *result,
                                                                gpointer user_data)
{
    auto *self = static_cast<DestructiveOperationController *>(user_data);
    GtkWindow *held_window = self->window_;

    GError *error = nullptr;
    const int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source_object), result, &error);
    if (!self->mutation_guard_.alive()) {
        g_clear_error(&error);
        g_object_unref(held_window);
        return;
    }
    self->dialog_busy_ = false;

    const std::string trash_uri = self->pending_trash_uri_;
    const std::string original_path = self->pending_original_path_;
    self->pending_trash_uri_.clear();
    self->pending_original_path_.clear();

    if (error != nullptr) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            self->show_alert("Restore", error->message);
        }
        g_error_free(error);
        self->update_action_state();
        g_object_unref(held_window);
        return;
    }

    if (choice == 1) {
        self->start_operation(Kind::RestoreReplace, {}, trash_uri, original_path);
    } else {
        self->update_action_state();
    }
    g_object_unref(held_window);
}

void DestructiveOperationController::start_operation(const Kind kind,
                                                     const std::string &source_path,
                                                     const std::string &trash_uri,
                                                     const std::string &original_path)
{
    if (busy_ || window_ == nullptr) {
        return;
    }

    const int kind_value = static_cast<int>(kind);
    const std::string &journal_source = source_path.empty() ? trash_uri : source_path;
    OperationJournal journal;
    const std::string journal_id = journal.begin(kind_name(kind_value),
                                                 journal_source,
                                                 original_path);
    if (journal_id.empty()) {
        show_alert("File operation",
                   "The durable operation journal could not be written, so the mutation was not started.");
        return;
    }

    auto *data = new DestructiveTaskData{
        kind_value, source_path, trash_uri, original_path, journal_id};
    busy_ = true;
    update_action_state();
    if (status_label_ != nullptr) {
        gtk_label_set_text(GTK_LABEL(status_label_), busy_text(data->kind));
    }

    mutation_guard_.begin();
    GTask *task = g_task_new(G_OBJECT(window_), nullptr,
                             &DestructiveOperationController::on_operation_finished, this);
    g_task_set_task_data(task, data, [](gpointer value) {
        delete static_cast<DestructiveTaskData *>(value);
    });
    g_task_run_in_thread(task, &DestructiveOperationController::on_operation_thread);
    g_object_unref(task);
}

void DestructiveOperationController::on_operation_thread(GTask *task,
                                                         gpointer source_object,
                                                         gpointer task_data,
                                                         GCancellable *cancellable)
{
    (void)source_object;
    (void)cancellable;
    const auto *data = static_cast<const DestructiveTaskData *>(task_data);
    RecoveryOperationEngine engine;

    OperationResult operation;
    switch (static_cast<Kind>(data->kind)) {
    case Kind::Trash:
        operation = engine.trash_item(data->source_path);
        break;
    case Kind::Restore:
        operation = engine.restore_item(data->trash_uri, data->original_path, false);
        break;
    case Kind::RestoreReplace:
        operation = engine.restore_item(data->trash_uri, data->original_path, true);
        break;
    case Kind::DeletePath:
        operation = engine.delete_item(data->source_path);
        break;
    case Kind::DeleteUri:
        operation = engine.delete_uri(data->trash_uri);
        break;
    }

    OperationJournal journal;
    if (!journal.finish(data->journal_id, kind_name(data->kind), operation)) {
        if (operation.ok()) {
            operation.status = OperationStatus::VerificationFailure;
            operation.phase = OperationPhase::Verify;
        }
        if (!operation.message.empty()) {
            operation.message += ' ';
        }
        operation.message += "The durable operation journal could not record completion.";
    }

    auto *returned = new OperationResult(std::move(operation));
    g_task_return_pointer(task, returned, [](gpointer value) {
        delete static_cast<OperationResult *>(value);
    });
}

void DestructiveOperationController::on_operation_finished(GObject *source_object,
                                                           GAsyncResult *result,
                                                           gpointer user_data)
{
    (void)source_object;
    auto *self = static_cast<DestructiveOperationController *>(user_data);
    GTask *task = G_TASK(result);

    const auto *task_data = static_cast<const DestructiveTaskData *>(g_task_get_task_data(task));
    const Kind completed_kind = task_data != nullptr
                                    ? static_cast<Kind>(task_data->kind)
                                    : Kind::Trash;
    const std::string trash_uri = task_data != nullptr ? task_data->trash_uri : std::string{};
    const std::string original_path = task_data != nullptr ? task_data->original_path : std::string{};

    GError *error = nullptr;
    std::unique_ptr<OperationResult> operation(
        static_cast<OperationResult *>(g_task_propagate_pointer(task, &error)));
    if (!self->mutation_guard_.finish()) {
        g_clear_error(&error);
        return;
    }
    self->busy_ = false;
    self->update_action_state();

    if (error != nullptr) {
        if (self->status_label_ != nullptr) {
            gtk_label_set_text(GTK_LABEL(self->status_label_), error->message);
        }
        self->show_alert("File operation", error->message);
        g_error_free(error);
        return;
    }
    if (!operation) {
        self->show_alert("File operation", "The operation did not return a result.");
        return;
    }

    if (operation->status == OperationStatus::DestinationConflict && completed_kind == Kind::Restore) {
        if (self->status_label_ != nullptr) {
            gtk_label_set_text(GTK_LABEL(self->status_label_), operation->message.c_str());
        }
        self->prompt_restore_replace(trash_uri, original_path, operation->message);
        return;
    }

    if (self->status_label_ != nullptr) {
        gtk_label_set_text(GTK_LABEL(self->status_label_), operation->message.c_str());
    }

    if (!operation->ok()) {
        self->show_alert(operation->status == OperationStatus::VerificationFailure
                             ? "Operation completed with a verification problem"
                             : operation->status == OperationStatus::UnsupportedCapability
                                   ? "Operation not supported safely"
                             : "File operation failed",
                         operation->message);
    }
}

void DestructiveOperationController::update_action_state()
{
    if (!mutation_guard_.alive()) return;
    if (menu_button_ == nullptr) {
        return;
    }

    const bool in_trash = current_location_is_trash();
    bool has_selection = false;

    if (in_trash) {
        std::string uri;
        std::string original;
        std::string name;
        has_selection = selected_trash_source(uri, original, name, false);
    } else {
        std::string path;
        std::string name;
        has_selection = selected_local_source(path, name);
    }

    const bool enabled = !busy_ && !dialog_busy_ && has_selection;
    gtk_widget_set_sensitive(menu_button_, enabled);
    if (trash_button_ != nullptr) {
        gtk_widget_set_visible(trash_button_, !in_trash);
        gtk_widget_set_sensitive(trash_button_, enabled && !in_trash);
    }
    if (restore_button_ != nullptr) {
        gtk_widget_set_visible(restore_button_, in_trash);
        gtk_widget_set_sensitive(restore_button_, enabled && in_trash);
    }
    if (delete_button_ != nullptr) {
        gtk_widget_set_sensitive(delete_button_, enabled);
    }
}

void DestructiveOperationController::close_menu()
{
    if (!mutation_guard_.alive()) return;
    if (popover_ != nullptr) {
        gtk_popover_popdown(GTK_POPOVER(popover_));
    }
}

void DestructiveOperationController::show_alert(const char *title,
                                                const std::string &detail) const
{
    if (window_ == nullptr) {
        return;
    }
    GtkAlertDialog *alert = gtk_alert_dialog_new("%s", title);
    gtk_alert_dialog_set_detail(alert, detail.c_str());
    gtk_alert_dialog_show(alert, window_);
    g_object_unref(alert);
}

bool DestructiveOperationController::current_location_is_trash() const
{
    if (directory_list_ == nullptr) {
        return false;
    }
    GFile *current = gtk_directory_list_get_file(directory_list_);
    if (current == nullptr) {
        return false;
    }
    char *uri = g_file_get_uri(current);
    if (uri == nullptr) {
        return false;
    }
    const std::string value = uri;
    g_free(uri);
    return value.rfind("trash:", 0U) == 0U;
}

bool DestructiveOperationController::selected_local_source(std::string &path,
                                                           std::string &name) const
{
    path.clear();
    name.clear();
    if (directory_list_ == nullptr || selection_ == nullptr) {
        return false;
    }

    GFile *parent = gtk_directory_list_get_file(directory_list_);
    if (parent == nullptr || !g_file_is_native(parent)) {
        return false;
    }

    const guint selected = gtk_single_selection_get_selected(selection_);
    if (selected == GTK_INVALID_LIST_POSITION) {
        return false;
    }

    GFileInfo *info = G_FILE_INFO(g_list_model_get_item(G_LIST_MODEL(directory_list_), selected));
    if (info == nullptr) {
        return false;
    }
    const char *raw_name = g_file_info_get_name(info);
    if (raw_name == nullptr || raw_name[0] == '\0') {
        g_object_unref(info);
        return false;
    }
    name = raw_name;
    g_object_unref(info);

    GFile *child = g_file_get_child(parent, name.c_str());
    char *native_path = g_file_get_path(child);
    g_object_unref(child);
    if (native_path == nullptr) {
        name.clear();
        return false;
    }

    path = native_path;
    g_free(native_path);
    return true;
}

bool DestructiveOperationController::selected_trash_source(std::string &uri,
                                                           std::string &original_path,
                                                           std::string &name,
                                                           const bool require_original) const
{
    uri.clear();
    original_path.clear();
    name.clear();
    if (!current_location_is_trash() || directory_list_ == nullptr || selection_ == nullptr) {
        return false;
    }

    const guint selected = gtk_single_selection_get_selected(selection_);
    if (selected == GTK_INVALID_LIST_POSITION) {
        return false;
    }

    GFileInfo *listed_info =
        G_FILE_INFO(g_list_model_get_item(G_LIST_MODEL(directory_list_), selected));
    if (listed_info == nullptr) {
        return false;
    }
    const char *raw_name = g_file_info_get_name(listed_info);
    if (raw_name == nullptr || raw_name[0] == '\0') {
        g_object_unref(listed_info);
        return false;
    }
    name = raw_name;
    g_object_unref(listed_info);

    GFile *parent = gtk_directory_list_get_file(directory_list_);
    GFile *child = g_file_get_child(parent, name.c_str());
    char *child_uri = g_file_get_uri(child);
    if (child_uri == nullptr) {
        g_object_unref(child);
        return false;
    }
    uri = child_uri;
    g_free(child_uri);

    if (!require_original) {
        g_object_unref(child);
        return true;
    }

    GError *query_error = nullptr;
    GFileInfo *info = g_file_query_info(child,
                                       G_FILE_ATTRIBUTE_TRASH_ORIG_PATH,
                                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                       nullptr,
                                       &query_error);
    g_object_unref(child);
    if (info == nullptr) {
        if (query_error != nullptr) {
            g_error_free(query_error);
        }
        return false;
    }

    const char *raw_original =
        g_file_info_get_attribute_byte_string(info, G_FILE_ATTRIBUTE_TRASH_ORIG_PATH);
    if (raw_original != nullptr) {
        original_path = raw_original;
    }
    g_object_unref(info);
    return !original_path.empty();
}

} // namespace infiltrator::files
