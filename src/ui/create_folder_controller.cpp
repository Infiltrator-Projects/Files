// SPDX-License-Identifier: GPL-3.0-or-later
#include "create_folder_controller.hpp"

#include "../core/operation_engine.hpp"
#include "../core/operation_journal.hpp"

#include <memory>
#include <string>

namespace infiltrator::files {

namespace {

struct CreateFolderTaskData {
    std::string parent_path;
    std::string parent_uri;
    std::string name;
    std::string journal_id;
    bool file{false};
};

} // namespace

CreateFolderController::CreateFolderController(GtkWindow *window,
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
        button_ = gtk_button_new_from_icon_name("folder-new-symbolic");
        gtk_widget_set_tooltip_text(button_, "New Folder (Ctrl+Shift+N)");
        gtk_header_bar_pack_end(GTK_HEADER_BAR(titlebar), button_);
        g_signal_connect(button_, "clicked", G_CALLBACK(&CreateFolderController::on_new_folder_clicked), this);
        new_file_button_ = gtk_button_new_from_icon_name("document-new-symbolic");
        gtk_widget_set_tooltip_text(new_file_button_, "New Empty File (Ctrl+Alt+N)");
        gtk_header_bar_pack_end(GTK_HEADER_BAR(titlebar), new_file_button_);
        g_signal_connect(new_file_button_, "clicked",
                         G_CALLBACK(&CreateFolderController::on_new_file_clicked), this);
    }

    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(&CreateFolderController::on_key_pressed), this);
    gtk_widget_add_controller(GTK_WIDGET(window_), keys);

    location_handler_ = g_signal_connect(directory_list_, "notify::file",
                                         G_CALLBACK(&CreateFolderController::on_location_changed), this);
    items_handler_ = g_signal_connect(directory_list_, "items-changed",
                                      G_CALLBACK(&CreateFolderController::on_items_changed), this);

    update_action_state();
    g_object_weak_ref(G_OBJECT(window_), &CreateFolderController::on_window_finalized, this);
}

CreateFolderController::~CreateFolderController()
{
    if (dialog_ != nullptr) {
        g_signal_handlers_disconnect_by_data(dialog_, this);
        gtk_window_destroy(GTK_WINDOW(dialog_));
        dialog_ = nullptr;
        entry_ = nullptr;
        dialog_message_ = nullptr;
    }
    if (directory_list_ != nullptr) {
        if (location_handler_ != 0U) {
            g_signal_handler_disconnect(directory_list_, location_handler_);
        }
        if (items_handler_ != 0U) {
            g_signal_handler_disconnect(directory_list_, items_handler_);
        }
        g_object_unref(directory_list_);
        directory_list_ = nullptr;
    }
    if (selection_ != nullptr) {
        g_object_unref(selection_);
        selection_ = nullptr;
    }
}

void CreateFolderController::on_window_finalized(gpointer user_data, GObject *where_object_was)
{
    (void)where_object_was;
    auto *self = static_cast<CreateFolderController *>(user_data);
    self->window_ = nullptr;
    delete self;
}

void CreateFolderController::on_new_folder_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    static_cast<CreateFolderController *>(user_data)->show_dialog(false);
}
void CreateFolderController::on_new_file_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    static_cast<CreateFolderController *>(user_data)->show_dialog(true);
}

gboolean CreateFolderController::on_key_pressed(GtkEventControllerKey *controller,
                                                 const guint keyval,
                                                 const guint keycode,
                                                 const GdkModifierType state,
                                                 gpointer user_data)
{
    (void)controller;
    (void)keycode;
    const bool control = (state & GDK_CONTROL_MASK) != 0;
    const bool shift = (state & GDK_SHIFT_MASK) != 0;
    if (control && (state & GDK_ALT_MASK) && !shift &&
        (keyval == GDK_KEY_n || keyval == GDK_KEY_N)) {
        static_cast<CreateFolderController *>(user_data)->show_dialog(true);
        return TRUE;
    }
    if (control && shift && (keyval == GDK_KEY_n || keyval == GDK_KEY_N)) {
        static_cast<CreateFolderController *>(user_data)->show_dialog();
        return TRUE;
    }
    return FALSE;
}

void CreateFolderController::on_location_changed(GObject *object,
                                                  GParamSpec *pspec,
                                                  gpointer user_data)
{
    (void)object;
    (void)pspec;
    auto *self = static_cast<CreateFolderController *>(user_data);
    self->pending_selection_name_.clear();
    self->update_action_state();
}

void CreateFolderController::on_items_changed(GListModel *model,
                                               const guint position,
                                               const guint removed,
                                               const guint added,
                                               gpointer user_data)
{
    (void)model;
    (void)position;
    (void)removed;
    (void)added;
    auto *self = static_cast<CreateFolderController *>(user_data);
    self->select_pending_item();
    self->update_status_count();
}

void CreateFolderController::on_dialog_destroy(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    auto *self = static_cast<CreateFolderController *>(user_data);
    self->dialog_ = nullptr;
    self->entry_ = nullptr;
    self->dialog_message_ = nullptr;
}

void CreateFolderController::on_dialog_cancel(GtkButton *button, gpointer user_data)
{
    (void)button;
    auto *self = static_cast<CreateFolderController *>(user_data);
    if (self->dialog_ != nullptr) {
        gtk_window_destroy(GTK_WINDOW(self->dialog_));
    }
}

void CreateFolderController::on_dialog_create(GtkButton *button, gpointer user_data)
{
    (void)button;
    static_cast<CreateFolderController *>(user_data)->submit_dialog();
}

void CreateFolderController::on_dialog_entry_activate(GtkEntry *entry, gpointer user_data)
{
    (void)entry;
    static_cast<CreateFolderController *>(user_data)->submit_dialog();
}

void CreateFolderController::show_dialog(const bool file)
{
    GtkWidget *target = file ? new_file_button_ : button_;
    if (busy_ || target == nullptr || !gtk_widget_get_sensitive(target)) {
        return;
    }
    if (dialog_ != nullptr) {
        gtk_window_present(GTK_WINDOW(dialog_));
        return;
    }

    creating_file_ = file;
    dialog_ = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog_), file ? "New Empty File" : "New Folder");
    gtk_window_set_transient_for(GTK_WINDOW(dialog_), window_);
    gtk_window_set_modal(GTK_WINDOW(dialog_), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(dialog_), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(dialog_), 420, -1);
    g_signal_connect(dialog_, "destroy", G_CALLBACK(&CreateFolderController::on_dialog_destroy), this);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_window_set_child(GTK_WINDOW(dialog_), box);

    GtkWidget *label = gtk_label_new(file ? "File name" : "Folder name");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    gtk_box_append(GTK_BOX(box), label);

    entry_ = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_), file ? "New File.txt" : "New Folder");
    gtk_box_append(GTK_BOX(box), entry_);
    g_signal_connect(entry_, "activate",
                     G_CALLBACK(&CreateFolderController::on_dialog_entry_activate), this);

    dialog_message_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(dialog_message_), 0.0F);
    gtk_label_set_wrap(GTK_LABEL(dialog_message_), TRUE);
    gtk_widget_add_css_class(dialog_message_, "error");
    gtk_widget_set_visible(dialog_message_, FALSE);
    gtk_box_append(GTK_BOX(box), dialog_message_);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(box), actions);

    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    GtkWidget *create = gtk_button_new_with_label("Create");
    gtk_widget_add_css_class(create, "suggested-action");
    gtk_box_append(GTK_BOX(actions), cancel);
    gtk_box_append(GTK_BOX(actions), create);
    g_signal_connect(cancel, "clicked", G_CALLBACK(&CreateFolderController::on_dialog_cancel), this);
    g_signal_connect(create, "clicked", G_CALLBACK(&CreateFolderController::on_dialog_create), this);

    gtk_window_present(GTK_WINDOW(dialog_));
    gtk_widget_grab_focus(entry_);
}

void CreateFolderController::submit_dialog()
{
    if (dialog_ == nullptr || entry_ == nullptr || busy_) {
        return;
    }

    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry_));
    const std::string name = text != nullptr ? text : "";
    std::string validation_error;
    if (!OperationEngine::validate_item_name(name, &validation_error)) {
        gtk_label_set_text(GTK_LABEL(dialog_message_), validation_error.c_str());
        gtk_widget_set_visible(dialog_message_, TRUE);
        return;
    }

    GFile *current = gtk_directory_list_get_file(directory_list_);
    if (current == nullptr || !g_file_is_native(current)) {
        show_alert("New Folder", "New folders are not yet supported in this location.");
        return;
    }

    char *path = g_file_get_path(current);
    char *uri = g_file_get_uri(current);
    if (path == nullptr || uri == nullptr) {
        g_free(path);
        g_free(uri);
        show_alert("New Folder", "The current location could not be resolved.");
        return;
    }

    OperationJournal journal;
    const std::string kind = creating_file_ ? "create-empty-file" : "create-folder";
    const std::string journal_id = journal.begin(kind, path, name);
    if (journal_id.empty()) {
        g_free(path);
        g_free(uri);
        show_alert("New Folder",
                   "The durable operation journal could not be written, so the folder was not created.");
        return;
    }

    auto *data = new CreateFolderTaskData{path, uri, name, journal_id, creating_file_};
    g_free(path);
    g_free(uri);

    gtk_window_destroy(GTK_WINDOW(dialog_));
    set_busy(true);
    if (status_label_ != nullptr) {
        gtk_label_set_text(GTK_LABEL(status_label_), data->file ? "Creating file…" : "Creating folder…");
    }

    mutation_guard_.begin();
    GTask *task = g_task_new(G_OBJECT(window_), nullptr,
                             &CreateFolderController::on_create_folder_finished, this);
    g_task_set_task_data(task, data, [](gpointer value) {
        delete static_cast<CreateFolderTaskData *>(value);
    });
    g_task_run_in_thread(task, &CreateFolderController::on_create_folder_thread);
    g_object_unref(task);
}

void CreateFolderController::on_create_folder_thread(GTask *task,
                                                      gpointer source_object,
                                                      gpointer task_data,
                                                      GCancellable *cancellable)
{
    (void)source_object;
    (void)cancellable;
    const auto *data = static_cast<const CreateFolderTaskData *>(task_data);
    OperationEngine engine;
    auto *result = new OperationResult(data->file
        ? engine.create_empty_file(data->parent_path, data->name)
        : engine.create_directory(data->parent_path, data->name));

    OperationJournal journal;
    if (!journal.finish(data->journal_id,
                        data->file ? "create-empty-file" : "create-folder", *result)) {
        if (result->ok()) {
            result->status = OperationStatus::VerificationFailure;
            result->phase = OperationPhase::Verify;
        }
        if (!result->message.empty()) {
            result->message += ' ';
        }
        result->message += "The durable operation journal could not record completion.";
    }

    g_task_return_pointer(task, result, [](gpointer value) {
        delete static_cast<OperationResult *>(value);
    });
}

void CreateFolderController::on_create_folder_finished(GObject *source_object,
                                                        GAsyncResult *result,
                                                        gpointer user_data)
{
    (void)source_object;
    auto *self = static_cast<CreateFolderController *>(user_data);
    auto *task = G_TASK(result);
    const auto *data = static_cast<const CreateFolderTaskData *>(g_task_get_task_data(task));

    GError *error = nullptr;
    std::unique_ptr<OperationResult> operation(
        static_cast<OperationResult *>(g_task_propagate_pointer(task, &error)));
    if (!self->mutation_guard_.finish()) {
        g_clear_error(&error);
        return;
    }
    self->set_busy(false);

    if (error != nullptr) {
        if (self->status_label_ != nullptr) {
            gtk_label_set_text(GTK_LABEL(self->status_label_), error->message);
        }
        self->show_alert(data != nullptr && data->file ? "New Empty File" : "New Folder", error->message);
        g_error_free(error);
        return;
    }
    if (!operation) {
        if (self->status_label_ != nullptr) {
            gtk_label_set_text(GTK_LABEL(self->status_label_), "The operation did not return a result.");
        }
        self->show_alert("New Folder", "The operation did not return a result.");
        return;
    }

    if (operation->changed && data != nullptr && self->current_location_is(data->parent_uri)) {
        self->pending_selection_name_ = operation->destination.filename().string();
        self->select_pending_item();
    }

    if (self->status_label_ != nullptr) {
        gtk_label_set_text(GTK_LABEL(self->status_label_), operation->message.c_str());
    }

    if (!operation->ok()) {
        self->show_alert(operation->status == OperationStatus::VerificationFailure
                             ? "Folder created with a verification problem"
                             : operation->status == OperationStatus::UnsupportedCapability
                                   ? "Folder operation not supported safely"
                             : "Could not create folder",
                         operation->message);
    }
}

void CreateFolderController::set_busy(const bool busy)
{
    if (!mutation_guard_.alive()) return;
    busy_ = busy;
    update_action_state();
}

void CreateFolderController::update_action_state()
{
    if (!mutation_guard_.alive()) return;
    if (button_ == nullptr || directory_list_ == nullptr) {
        return;
    }
    GFile *current = gtk_directory_list_get_file(directory_list_);
    const bool can_create = !busy_ && current != nullptr && g_file_is_native(current);
    gtk_widget_set_sensitive(button_, can_create);
    if (new_file_button_ != nullptr) gtk_widget_set_sensitive(new_file_button_, can_create);
}

void CreateFolderController::select_pending_item()
{
    if (!mutation_guard_.alive()) return;
    if (pending_selection_name_.empty() || directory_list_ == nullptr || selection_ == nullptr) {
        return;
    }

    const guint count = g_list_model_get_n_items(G_LIST_MODEL(directory_list_));
    for (guint index = 0U; index < count; ++index) {
        GFileInfo *info = G_FILE_INFO(g_list_model_get_item(G_LIST_MODEL(directory_list_), index));
        if (info == nullptr) {
            continue;
        }
        const char *name = g_file_info_get_name(info);
        const bool match = name != nullptr && pending_selection_name_ == name;
        g_object_unref(info);
        if (match) {
            gtk_single_selection_set_selected(selection_, index);
            pending_selection_name_.clear();
            return;
        }
    }
}

void CreateFolderController::update_status_count()
{
    if (!mutation_guard_.alive()) return;
    if (status_label_ == nullptr || directory_list_ == nullptr ||
        gtk_directory_list_is_loading(directory_list_)) {
        return;
    }
    if (gtk_directory_list_get_error(directory_list_) != nullptr) {
        return;
    }
    const guint count = g_list_model_get_n_items(G_LIST_MODEL(directory_list_));
    const std::string text = std::to_string(count) + (count == 1U ? " item" : " items");
    gtk_label_set_text(GTK_LABEL(status_label_), text.c_str());
}

void CreateFolderController::show_alert(const char *title, const std::string &detail) const
{
    if (window_ == nullptr) {
        return;
    }
    GtkAlertDialog *alert = gtk_alert_dialog_new("%s", title);
    gtk_alert_dialog_set_detail(alert, detail.c_str());
    gtk_alert_dialog_show(alert, window_);
    g_object_unref(alert);
}

bool CreateFolderController::current_location_is(const std::string &uri) const
{
    if (directory_list_ == nullptr) {
        return false;
    }
    GFile *current = gtk_directory_list_get_file(directory_list_);
    if (current == nullptr) {
        return false;
    }
    char *current_uri = g_file_get_uri(current);
    if (current_uri == nullptr) {
        return false;
    }
    const bool same = uri == current_uri;
    g_free(current_uri);
    return same;
}

} // namespace infiltrator::files
