// SPDX-License-Identifier: GPL-3.0-or-later
#include "item_operation_controller.hpp"
#include "file_manager_window.hpp"

#include "../core/operation_engine.hpp"
#include "../core/operation_journal.hpp"
#include "../core/recovery_operation_engine.hpp"
#include "../core/transfer_operation_engine.hpp"

#include <memory>
#include <string>
#include <utility>

namespace infiltrator::files {

namespace {

struct ItemOperationTaskData {
    int kind{0};
    std::string source_path;
    std::string destination_parent;
    std::string new_name;
    std::string journal_id;
    std::shared_ptr<TransferControl> control;
};

const char *busy_text(const int kind)
{
    switch (kind) {
    case 0:
        return "Renaming item…";
    case 1:
        return "Copying item…";
    case 2:
        return "Moving item…";
    case 3:
        return "Copying item with a new name…";
    case 4:
        return "Moving item with a new name…";
    case 5:
        return "Replacing item by copy…";
    case 6:
        return "Replacing item by move…";
    case 7:
        return "Changing permissions…";
    default:
        return "Working…";
    }
}

} // namespace

ItemOperationController::ItemOperationController(GtkWindow *window,
                                                 GtkDirectoryList *directory_list,
                                                 GtkSingleSelection *selection,
                                                 GtkWidget *status_label)
    : mutation_guard_(window, directory_list),
      progress_view_(window, mutation_guard_, status_label, "File operation progress"), window_(window), directory_list_(directory_list), selection_(selection),
      status_label_(status_label)
{
    if (window_ == nullptr || directory_list_ == nullptr || selection_ == nullptr) {
        return;
    }

    g_object_ref(directory_list_);
    g_object_ref(selection_);
    g_object_set_data(G_OBJECT(window_), "ifm-item-controller", this);

    GtkWidget *titlebar = gtk_window_get_titlebar(window_);
    if (GTK_IS_HEADER_BAR(titlebar)) {
        menu_button_ = gtk_menu_button_new();
        gtk_menu_button_set_child(GTK_MENU_BUTTON(menu_button_), gtk_label_new("Actions"));
        gtk_widget_set_tooltip_text(menu_button_, "Selected item actions");

        popover_ = gtk_popover_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_margin_start(box, 8);
        gtk_widget_set_margin_end(box, 8);
        gtk_widget_set_margin_top(box, 8);
        gtk_widget_set_margin_bottom(box, 8);

        rename_button_ = gtk_button_new_with_label("Rename");
        copy_button_ = gtk_button_new_with_label("Copy to…");
        move_button_ = gtk_button_new_with_label("Move to…");
        permissions_button_ = gtk_button_new_with_label("Edit Permissions…");
        for (GtkWidget *button : {rename_button_, copy_button_, move_button_, permissions_button_}) {
            gtk_widget_add_css_class(button, "flat");
            gtk_widget_set_halign(button, GTK_ALIGN_FILL);
            gtk_box_append(GTK_BOX(box), button);
        }

        g_signal_connect(rename_button_, "clicked",
                         G_CALLBACK(&ItemOperationController::on_rename_clicked), this);
        g_signal_connect(copy_button_, "clicked",
                         G_CALLBACK(&ItemOperationController::on_copy_clicked), this);
        g_signal_connect(move_button_, "clicked",
                         G_CALLBACK(&ItemOperationController::on_move_clicked), this);
        g_signal_connect(permissions_button_, "clicked",
                         G_CALLBACK(&ItemOperationController::on_permissions_clicked), this);

        gtk_popover_set_child(GTK_POPOVER(popover_), box);
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu_button_), popover_);
        gtk_header_bar_pack_end(GTK_HEADER_BAR(titlebar), menu_button_);

    }

    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(&ItemOperationController::on_key_pressed), this);
    gtk_widget_add_controller(GTK_WIDGET(window_), keys);

    selection_handler_ = g_signal_connect(selection_, "notify::selected",
                                          G_CALLBACK(&ItemOperationController::on_selection_changed), this);
    location_handler_ = g_signal_connect(directory_list_, "notify::file",
                                         G_CALLBACK(&ItemOperationController::on_location_changed), this);
    items_handler_ = g_signal_connect(directory_list_, "items-changed",
                                      G_CALLBACK(&ItemOperationController::on_items_changed), this);

    update_action_state();
    g_object_weak_ref(G_OBJECT(window_), &ItemOperationController::on_window_finalized, this);
}

ItemOperationController::~ItemOperationController()
{
    if (window_ != nullptr) g_object_set_data(G_OBJECT(window_), "ifm-item-controller", nullptr);
    if (current_control_) {
        current_control_->request_cancel();
    }
    if (rename_dialog_ != nullptr) {
        g_signal_handlers_disconnect_by_data(rename_dialog_, this);
        gtk_window_destroy(GTK_WINDOW(rename_dialog_));
        rename_dialog_ = nullptr;
    }
    if (permissions_dialog_ != nullptr) {
        g_signal_handlers_disconnect_by_data(permissions_dialog_, this);
        gtk_window_destroy(GTK_WINDOW(permissions_dialog_));
        permissions_dialog_ = nullptr;
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
        if (selection_handler_ != 0U) {
            g_signal_handler_disconnect(selection_, selection_handler_);
        }
        g_object_unref(selection_);
        selection_ = nullptr;
    }
}

const char *ItemOperationController::kind_name(const Kind kind) noexcept
{
    switch (kind) {
    case Kind::Rename:
        return "rename";
    case Kind::Copy:
        return "copy";
    case Kind::Move:
        return "move";
    case Kind::KeepBothCopy:
        return "copy-keep-both";
    case Kind::KeepBothMove:
        return "move-keep-both";
    case Kind::ReplaceCopy:
        return "copy-replace";
    case Kind::ReplaceMove:
        return "move-replace";
    case Kind::Permissions:
        return "permissions";
    }
    return "unknown";
}

bool ItemOperationController::kind_is_transfer(const Kind kind) noexcept
{
    return kind == Kind::Copy || kind == Kind::Move ||
           kind == Kind::KeepBothCopy || kind == Kind::KeepBothMove;
}

void ItemOperationController::on_window_finalized(gpointer user_data, GObject *where_object_was)
{
    (void)where_object_was;
    auto *self = static_cast<ItemOperationController *>(user_data);
    self->window_ = nullptr;
    delete self;
}

void ItemOperationController::on_rename_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    auto *self = static_cast<ItemOperationController *>(user_data);
    self->close_menu();
    self->show_rename_dialog();
}

void ItemOperationController::on_permissions_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    auto *self = static_cast<ItemOperationController *>(user_data);
    self->close_menu();
    self->show_permissions_dialog();
}

void ItemOperationController::on_copy_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    auto *self = static_cast<ItemOperationController *>(user_data);
    self->close_menu();
    self->choose_destination(Kind::Copy);
}

void ItemOperationController::on_move_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    auto *self = static_cast<ItemOperationController *>(user_data);
    self->close_menu();
    self->choose_destination(Kind::Move);
}

gboolean ItemOperationController::on_key_pressed(GtkEventControllerKey *controller,
                                                  const guint keyval,
                                                  const guint keycode,
                                                  const GdkModifierType state,
                                                  gpointer user_data)
{
    (void)controller;
    (void)keycode;
    (void)state;
    if (keyval == GDK_KEY_F2) {
        static_cast<ItemOperationController *>(user_data)->show_rename_dialog();
        return TRUE;
    }
    return FALSE;
}

void ItemOperationController::on_selection_changed(GObject *object,
                                                   GParamSpec *pspec,
                                                   gpointer user_data)
{
    (void)object;
    (void)pspec;
    static_cast<ItemOperationController *>(user_data)->update_action_state();
}

void ItemOperationController::on_location_changed(GObject *object,
                                                  GParamSpec *pspec,
                                                  gpointer user_data)
{
    (void)object;
    (void)pspec;
    auto *self = static_cast<ItemOperationController *>(user_data);
    self->pending_selection_name_.clear();
    self->update_action_state();
}

void ItemOperationController::on_items_changed(GListModel *model,
                                               const guint position,
                                               const guint removed,
                                               const guint added,
                                               gpointer user_data)
{
    (void)model;
    (void)position;
    (void)removed;
    (void)added;
    auto *self = static_cast<ItemOperationController *>(user_data);
    self->select_pending_item();
    self->update_action_state();
}

void ItemOperationController::on_rename_dialog_destroy(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    auto *self = static_cast<ItemOperationController *>(user_data);
    self->rename_dialog_ = nullptr;
    self->rename_entry_ = nullptr;
    self->rename_message_ = nullptr;
    self->rename_source_path_.clear();
}

void ItemOperationController::on_rename_cancel(GtkButton *button, gpointer user_data)
{
    (void)button;
    auto *self = static_cast<ItemOperationController *>(user_data);
    if (self->rename_dialog_ != nullptr) {
        gtk_window_destroy(GTK_WINDOW(self->rename_dialog_));
    }
}

void ItemOperationController::on_rename_submit(GtkButton *button, gpointer user_data)
{
    (void)button;
    static_cast<ItemOperationController *>(user_data)->submit_rename();
}

void ItemOperationController::on_rename_entry_activate(GtkEntry *entry, gpointer user_data)
{
    (void)entry;
    static_cast<ItemOperationController *>(user_data)->submit_rename();
}

void ItemOperationController::on_permissions_dialog_destroy(GtkWidget *, gpointer data)
{
    auto *self = static_cast<ItemOperationController *>(data);
    self->permissions_dialog_ = nullptr;
    self->permissions_entry_ = nullptr;
    self->permissions_grid_ = nullptr;
    self->permission_sync_ = false;
    self->permissions_message_ = nullptr;
    self->permissions_source_path_.clear();
}

void ItemOperationController::on_permissions_submit(GtkButton *, gpointer data)
{
    static_cast<ItemOperationController *>(data)->submit_permissions();
}

void ItemOperationController::on_permissions_cancel(GtkButton *, gpointer data)
{
    auto *self = static_cast<ItemOperationController *>(data);
    if (self->permissions_dialog_ != nullptr)
        gtk_window_destroy(GTK_WINDOW(self->permissions_dialog_));
}

void ItemOperationController::on_permissions_entry_activate(GtkEntry *, gpointer data)
{
    static_cast<ItemOperationController *>(data)->submit_permissions();
}

void ItemOperationController::on_permission_toggle(GtkCheckButton *, gpointer data)
{
    auto *self = static_cast<ItemOperationController *>(data);
    if (self->permission_sync_ || self->permissions_grid_ == nullptr ||
        self->permissions_entry_ == nullptr) return;
    guint mode = 0U;
    for (GtkWidget *child = gtk_widget_get_first_child(self->permissions_grid_);
         child != nullptr; child = gtk_widget_get_next_sibling(child)) {
        if (GTK_IS_CHECK_BUTTON(child) && gtk_check_button_get_active(GTK_CHECK_BUTTON(child)))
            mode |= GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(child), "ifm-mode-bit"));
    }
    char *text = g_strdup_printf("%03o", mode);
    self->permission_sync_ = true;
    gtk_editable_set_text(GTK_EDITABLE(self->permissions_entry_), text);
    self->permission_sync_ = false;
    g_free(text);
}

void ItemOperationController::on_permissions_text_changed(GtkEditable *field, gpointer data)
{
    auto *self = static_cast<ItemOperationController *>(data);
    if (self->permission_sync_ || self->permissions_grid_ == nullptr) return;
    const char *text = gtk_editable_get_text(field);
    if (text == nullptr) return;
    const std::string mode(text);
    if (!(mode.size() == 3U || (mode.size() == 4U && mode[0] == '0')))
        return;
    guint bits = 0U;
    for (char c : mode) {
        if (c < '0' || c > '7') return;
        bits = bits * 8U + static_cast<guint>(c - '0');
    }
    self->permission_sync_ = true;
    for (GtkWidget *child = gtk_widget_get_first_child(self->permissions_grid_);
         child != nullptr; child = gtk_widget_get_next_sibling(child)) {
        if (!GTK_IS_CHECK_BUTTON(child)) continue;
        const guint bit = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(child), "ifm-mode-bit"));
        gtk_check_button_set_active(GTK_CHECK_BUTTON(child), (bits & bit) != 0U);
    }
    self->permission_sync_ = false;
}

void ItemOperationController::show_permissions_dialog()
{
    if (busy_ || chooser_busy_ || window_ == nullptr) return;
    if (permissions_dialog_ != nullptr) {
        gtk_window_present(GTK_WINDOW(permissions_dialog_));
        return;
    }

    std::string source_path;
    std::string name;
    if (!selected_source(source_path, name)) return;
    permissions_source_path_ = source_path;
    GtkWidget *dialog = gtk_window_new();
    permissions_dialog_ = dialog;
    gtk_window_set_title(GTK_WINDOW(dialog), "Edit Permissions");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), window_);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 400, -1);
    g_signal_connect(dialog, "destroy",
                     G_CALLBACK(&ItemOperationController::on_permissions_dialog_destroy), this);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_window_set_child(GTK_WINDOW(dialog), box);
    GtkWidget *heading = gtk_label_new(name.c_str());
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0F);
    gtk_box_append(GTK_BOX(box), heading);
    GtkWidget *instruction = gtk_label_new(
        "Owner / group / other permissions (octal: e.g. 0644 or 0755). "
        "Native regular files and directories only. Special bits, symlinks, "
        "ACLs and privilege elevation are not changed.");
    gtk_label_set_wrap(GTK_LABEL(instruction), TRUE);
    gtk_label_set_xalign(GTK_LABEL(instruction), 0.0F);
    gtk_box_append(GTK_BOX(box), instruction);

    permissions_entry_ = gtk_entry_new();
    guint32 presented_mode = 0U;
    bool has_mode = false;
    bool ordinary_mode = false;
    // The already enumerated GFileInfo supplies the displayed mode. Never
    // query a potentially slow filesystem synchronously from the UI thread.
    const guint selected = gtk_single_selection_get_selected(selection_);
    GFileInfo *info = selected != GTK_INVALID_LIST_POSITION
        ? G_FILE_INFO(g_list_model_get_item(G_LIST_MODEL(directory_list_), selected))
        : nullptr;
    if (info != nullptr && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_MODE)) {
        const guint32 mode = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE);
        presented_mode = mode & 0777U;
        has_mode = true;
        ordinary_mode = (mode & 07000U) == 0U;
        char *text = g_strdup_printf("%04o", mode & 07777U);
        gtk_editable_set_text(GTK_EDITABLE(permissions_entry_), text);
        g_free(text);
    }
    g_clear_object(&info);
    gtk_box_append(GTK_BOX(box), permissions_entry_);
    permissions_grid_ = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(permissions_grid_), 7);
    gtk_grid_set_column_spacing(GTK_GRID(permissions_grid_), 14);
    constexpr const char *groups[] = {"Owner", "Group", "Others"};
    constexpr const char *flags[] = {"Read", "Write", "Execute"};
    constexpr guint masks[] = {0400U, 0200U, 0100U,
                               0040U, 0020U, 0010U,
                               0004U, 0002U, 0001U};
    for (guint row = 0; row != 3U; ++row) {
        GtkWidget *group = gtk_label_new(groups[row]);
        gtk_widget_set_halign(group, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(permissions_grid_), group, 0, static_cast<int>(row), 1, 1);
        for (guint col = 0; col != 3U; ++col) {
            GtkWidget *check = gtk_check_button_new_with_label(flags[col]);
            const guint mask = masks[row * 3U + col];
            g_object_set_data(G_OBJECT(check), "ifm-mode-bit", GUINT_TO_POINTER(mask));
            gtk_check_button_set_active(GTK_CHECK_BUTTON(check), (presented_mode & mask) != 0U);
            gtk_grid_attach(GTK_GRID(permissions_grid_), check,
                            static_cast<int>(col + 1U), static_cast<int>(row), 1, 1);
            g_signal_connect(check, "toggled", G_CALLBACK(on_permission_toggle), this);
        }
    }
    // Checkbox editing must never silently clear setuid, setgid or sticky bits.
    gtk_widget_set_sensitive(permissions_grid_, has_mode && ordinary_mode);
    gtk_box_append(GTK_BOX(box), permissions_grid_);
    g_signal_connect(permissions_entry_, "changed", G_CALLBACK(on_permissions_text_changed), this);
    g_signal_connect(permissions_entry_, "activate",
                     G_CALLBACK(&ItemOperationController::on_permissions_entry_activate), this);
    permissions_message_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(permissions_message_), 0.0F);
    gtk_label_set_wrap(GTK_LABEL(permissions_message_), TRUE);
    gtk_widget_add_css_class(permissions_message_, "error");
    gtk_widget_set_visible(permissions_message_, FALSE);
    gtk_box_append(GTK_BOX(box), permissions_message_);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(box), actions);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    GtkWidget *apply = gtk_button_new_with_label("Apply Permissions");
    gtk_widget_add_css_class(apply, "suggested-action");
    gtk_box_append(GTK_BOX(actions), cancel);
    gtk_box_append(GTK_BOX(actions), apply);
    g_signal_connect(cancel, "clicked",
                     G_CALLBACK(&ItemOperationController::on_permissions_cancel), this);
    g_signal_connect(apply, "clicked",
                     G_CALLBACK(&ItemOperationController::on_permissions_submit), this);
    gtk_window_present(GTK_WINDOW(dialog));
    gtk_widget_grab_focus(permissions_entry_);
}

void ItemOperationController::submit_permissions()
{
    if (permissions_dialog_ == nullptr || permissions_entry_ == nullptr ||
        permissions_source_path_.empty() || busy_) return;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(permissions_entry_));
    const std::string mode = text != nullptr ? text : "";
    const bool width_ok = mode.size() == 3U ||
        (mode.size() == 4U && mode[0] == '0');
    bool digits_ok = width_ok;
    for (const char digit : mode) {
        if (digit < '0' || digit > '7') digits_ok = false;
    }
    if (!digits_ok) {
        gtk_label_set_text(GTK_LABEL(permissions_message_),
                           "Enter exactly three octal digits, optionally prefixed by 0 (e.g. 0644).");
        gtk_widget_set_visible(permissions_message_, TRUE);
        return;
    }
    const std::string source = permissions_source_path_;
    gtk_window_destroy(GTK_WINDOW(permissions_dialog_));
    start_operation(Kind::Permissions, source, {}, mode);
}

void ItemOperationController::show_rename_dialog()
{
    if (busy_ || chooser_busy_) {
        return;
    }
    if (rename_dialog_ != nullptr) {
        gtk_window_present(GTK_WINDOW(rename_dialog_));
        return;
    }
    auto *manager = static_cast<FileManagerWindow *>(
        g_object_get_data(G_OBJECT(window_), "ifm-window-controller"));
    if (manager != nullptr && manager->begin_inline_rename()) return;

    std::string source_path;
    std::string current_name;
    if (!selected_source(source_path, current_name)) {
        return;
    }
    rename_source_path_ = source_path;

    rename_dialog_ = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(rename_dialog_), "Rename");
    gtk_window_set_transient_for(GTK_WINDOW(rename_dialog_), window_);
    gtk_window_set_modal(GTK_WINDOW(rename_dialog_), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(rename_dialog_), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(rename_dialog_), 420, -1);
    g_signal_connect(rename_dialog_, "destroy",
                     G_CALLBACK(&ItemOperationController::on_rename_dialog_destroy), this);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_window_set_child(GTK_WINDOW(rename_dialog_), box);

    GtkWidget *label = gtk_label_new("New name");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    gtk_box_append(GTK_BOX(box), label);

    rename_entry_ = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(rename_entry_), current_name.c_str());
    gtk_box_append(GTK_BOX(box), rename_entry_);
    g_signal_connect(rename_entry_, "activate",
                     G_CALLBACK(&ItemOperationController::on_rename_entry_activate), this);

    rename_message_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(rename_message_), 0.0F);
    gtk_label_set_wrap(GTK_LABEL(rename_message_), TRUE);
    gtk_widget_add_css_class(rename_message_, "error");
    gtk_widget_set_visible(rename_message_, FALSE);
    gtk_box_append(GTK_BOX(box), rename_message_);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(box), actions);

    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    GtkWidget *rename = gtk_button_new_with_label("Rename");
    gtk_widget_add_css_class(rename, "suggested-action");
    gtk_box_append(GTK_BOX(actions), cancel);
    gtk_box_append(GTK_BOX(actions), rename);
    g_signal_connect(cancel, "clicked", G_CALLBACK(&ItemOperationController::on_rename_cancel), this);
    g_signal_connect(rename, "clicked", G_CALLBACK(&ItemOperationController::on_rename_submit), this);

    gtk_window_present(GTK_WINDOW(rename_dialog_));
    gtk_widget_grab_focus(rename_entry_);
    gtk_editable_select_region(GTK_EDITABLE(rename_entry_), 0, -1);
}

void ItemOperationController::submit_rename()
{
    if (rename_dialog_ == nullptr || rename_entry_ == nullptr || rename_source_path_.empty() || busy_) {
        return;
    }

    const char *text = gtk_editable_get_text(GTK_EDITABLE(rename_entry_));
    const std::string new_name = text != nullptr ? text : "";
    std::string validation_error;
    if (!OperationEngine::validate_item_name(new_name, &validation_error)) {
        gtk_label_set_text(GTK_LABEL(rename_message_), validation_error.c_str());
        gtk_widget_set_visible(rename_message_, TRUE);
        return;
    }

    const std::string source_path = rename_source_path_;
    gtk_window_destroy(GTK_WINDOW(rename_dialog_));
    start_operation(Kind::Rename, source_path, {}, new_name);
}

void ItemOperationController::rename_selected_to(const std::string &new_name)
{
    if (busy_ || chooser_busy_) return;
    std::string path, current_name;
    if (!selected_source(path, current_name) || new_name == current_name) return;
    std::string error;
    if (!OperationEngine::validate_item_name(new_name, &error)) {
        show_alert("Rename", error);
        return;
    }
    start_operation(Kind::Rename, path, {}, new_name);
}

void ItemOperationController::choose_destination(const Kind kind)
{
    if (busy_ || chooser_busy_ || window_ == nullptr) {
        return;
    }

    std::string source_path;
    std::string name;
    if (!selected_source(source_path, name)) {
        return;
    }

    chooser_busy_ = true;
    chooser_kind_ = kind;
    chooser_source_path_ = source_path;
    update_action_state();

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, kind == Kind::Copy ? "Copy to" : "Move to");
    if (GFile *current = gtk_directory_list_get_file(directory_list_); current != nullptr) {
        gtk_file_dialog_set_initial_folder(dialog, current);
    }

    g_object_ref(window_);
    gtk_file_dialog_select_folder(dialog,
                                  window_,
                                  nullptr,
                                  &ItemOperationController::on_destination_chosen,
                                  this);
    g_object_unref(dialog);
}

void ItemOperationController::on_destination_chosen(GObject *source_object,
                                                    GAsyncResult *result,
                                                    gpointer user_data)
{
    auto *self = static_cast<ItemOperationController *>(user_data);
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
        self->chooser_source_path_.clear();
        self->update_action_state();
        g_object_unref(held_window);
        return;
    }
    if (folder == nullptr || !g_file_is_native(folder)) {
        if (folder != nullptr) {
            g_object_unref(folder);
        }
        self->show_alert("Choose destination", "Copy and move currently require a local destination.");
        self->chooser_source_path_.clear();
        self->update_action_state();
        g_object_unref(held_window);
        return;
    }

    char *destination_path = g_file_get_path(folder);
    g_object_unref(folder);
    if (destination_path == nullptr) {
        self->show_alert("Choose destination", "The destination folder could not be resolved.");
        self->chooser_source_path_.clear();
        self->update_action_state();
        g_object_unref(held_window);
        return;
    }

    const std::string source_path = self->chooser_source_path_;
    const std::string destination_parent = destination_path;
    const Kind kind = self->chooser_kind_;
    self->chooser_source_path_.clear();
    g_free(destination_path);

    self->start_operation(kind, source_path, destination_parent, {});
    g_object_unref(held_window);
}

void ItemOperationController::prompt_conflict(const Kind completed_kind,
                                              const std::string &source_path,
                                              const std::string &destination_parent,
                                              const std::string &detail)
{
    if (window_ == nullptr || chooser_busy_ || source_path.empty() || destination_parent.empty()) {
        return;
    }

    chooser_busy_ = true;
    pending_conflict_kind_ = completed_kind;
    pending_conflict_source_path_ = source_path;
    pending_conflict_destination_parent_ = destination_parent;
    update_action_state();

    GtkAlertDialog *dialog = gtk_alert_dialog_new("An item with that name already exists");
    const std::string explanation = detail +
        " Choose Skip to leave both locations unchanged, Keep Both to create a unique destination name, or Replace to stage the existing destination and replace it recoverably.";
    gtk_alert_dialog_set_detail(dialog, explanation.c_str());
    const char *buttons[] = {"Cancel", "Skip", "Keep Both", "Replace", nullptr};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 2);

    g_object_ref(window_);
    gtk_alert_dialog_choose(dialog,
                            window_,
                            nullptr,
                            &ItemOperationController::on_conflict_chosen,
                            this);
    g_object_unref(dialog);
}

void ItemOperationController::on_conflict_chosen(GObject *source_object,
                                                  GAsyncResult *result,
                                                  gpointer user_data)
{
    auto *self = static_cast<ItemOperationController *>(user_data);
    GtkWindow *held_window = self->window_;

    GError *error = nullptr;
    const int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source_object), result, &error);
    if (!self->mutation_guard_.alive()) {
        g_clear_error(&error);
        g_object_unref(held_window);
        return;
    }
    self->chooser_busy_ = false;

    const Kind original_kind = self->pending_conflict_kind_;
    const std::string source_path = self->pending_conflict_source_path_;
    const std::string destination_parent = self->pending_conflict_destination_parent_;
    self->pending_conflict_source_path_.clear();
    self->pending_conflict_destination_parent_.clear();

    if (error != nullptr) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            self->show_alert("Resolve conflict", error->message);
        }
        g_error_free(error);
        self->update_action_state();
        g_object_unref(held_window);
        return;
    }

    if (choice == 1) {
        if (self->status_label_ != nullptr) {
            gtk_label_set_text(GTK_LABEL(self->status_label_), "Skipped conflicting item.");
        }
        self->update_action_state();
    } else if (choice == 2) {
        self->start_operation(original_kind == Kind::Move ? Kind::KeepBothMove : Kind::KeepBothCopy,
                              source_path,
                              destination_parent,
                              {});
    } else if (choice == 3) {
        self->start_operation(original_kind == Kind::Move ? Kind::ReplaceMove : Kind::ReplaceCopy,
                              source_path,
                              destination_parent,
                              {});
    } else {
        self->update_action_state();
    }
    g_object_unref(held_window);
}

void ItemOperationController::start_operation(const Kind kind,
                                              const std::string &source_path,
                                              const std::string &destination_parent,
                                              const std::string &new_name)
{
    if (busy_ || source_path.empty() || window_ == nullptr || !mutation_guard_.begin()) {
        return;
    }

    OperationJournal journal;
    const std::string journal_destination =
        kind == Kind::Rename || kind == Kind::Permissions ? new_name : destination_parent;
    const std::string journal_id = journal.begin(kind_name(kind),
                                                 source_path,
                                                 journal_destination);
    if (journal_id.empty()) {
        (void)mutation_guard_.finish();
        show_alert("File operation",
                   "The durable operation journal could not be written, so the mutation was not started.");
        return;
    }

    std::shared_ptr<TransferControl> control;
    if (kind_is_transfer(kind)) {
        control = std::make_shared<TransferControl>();
    }
    current_control_ = control;

    auto *data = new ItemOperationTaskData{
        static_cast<int>(kind), source_path, destination_parent, new_name, journal_id, control};

    set_busy(true);
    if (status_label_ != nullptr) {
        gtk_label_set_text(GTK_LABEL(status_label_), busy_text(data->kind));
    }

    GTask *task = g_task_new(G_OBJECT(window_), nullptr,
                             &ItemOperationController::on_operation_finished, this);
    g_task_set_task_data(task, data, [](gpointer value) {
        delete static_cast<ItemOperationTaskData *>(value);
    });
    g_task_run_in_thread(task, &ItemOperationController::on_operation_thread);
    g_object_unref(task);
}

void ItemOperationController::on_operation_thread(GTask *task,
                                                  gpointer source_object,
                                                  gpointer task_data,
                                                  GCancellable *cancellable)
{
    (void)source_object;
    (void)cancellable;
    const auto *data = static_cast<const ItemOperationTaskData *>(task_data);
    const Kind kind = static_cast<Kind>(data->kind);

    OperationEngine ordinary;
    TransferOperationEngine transfer;
    RecoveryOperationEngine recovery;

    OperationResult operation;
    switch (kind) {
    case Kind::Rename:
        operation = ordinary.rename_item(data->source_path, data->new_name);
        break;
    case Kind::Permissions: {
        unsigned int permissions = 0U;
        for (const char digit : data->new_name) permissions = permissions * 8U +
            static_cast<unsigned int>(digit - '0');
        operation = ordinary.change_permissions(data->source_path, permissions);
        break;
    }
    case Kind::Copy:
        operation = transfer.copy_item(data->source_path,
                                       data->destination_parent,
                                       ConflictPolicy::Fail,
                                       data->control.get());
        break;
    case Kind::Move:
        operation = transfer.move_item(data->source_path,
                                       data->destination_parent,
                                       ConflictPolicy::Fail,
                                       data->control.get());
        break;
    case Kind::KeepBothCopy:
        operation = transfer.copy_item(data->source_path,
                                       data->destination_parent,
                                       ConflictPolicy::KeepBoth,
                                       data->control.get());
        break;
    case Kind::KeepBothMove:
        operation = transfer.move_item(data->source_path,
                                       data->destination_parent,
                                       ConflictPolicy::KeepBoth,
                                       data->control.get());
        break;
    case Kind::ReplaceCopy:
        operation = recovery.replace_copy(data->source_path, data->destination_parent, data->control.get());
        break;
    case Kind::ReplaceMove:
        operation = recovery.replace_move(data->source_path, data->destination_parent, data->control.get());
        break;
    }

    OperationJournal journal;
    if (!journal.finish(data->journal_id, kind_name(kind), operation)) {
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

void ItemOperationController::on_operation_finished(GObject *source_object,
                                                    GAsyncResult *result,
                                                    gpointer user_data)
{
    (void)source_object;
    auto *self = static_cast<ItemOperationController *>(user_data);
    GTask *task = G_TASK(result);

    const auto *task_data = static_cast<const ItemOperationTaskData *>(g_task_get_task_data(task));
    const Kind completed_kind = task_data != nullptr
                                    ? static_cast<Kind>(task_data->kind)
                                    : Kind::Rename;
    const std::string source_path = task_data != nullptr ? task_data->source_path : std::string{};
    const std::string destination_parent =
        task_data != nullptr ? task_data->destination_parent : std::string{};

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
        self->show_alert("File operation", error->message);
        g_error_free(error);
        return;
    }
    if (!operation) {
        self->show_alert("File operation", "The operation did not return a result.");
        return;
    }

    if (operation->status == OperationStatus::DestinationConflict &&
        (completed_kind == Kind::Copy || completed_kind == Kind::Move)) {
        if (self->status_label_ != nullptr) {
            gtk_label_set_text(GTK_LABEL(self->status_label_), operation->message.c_str());
        }
        self->prompt_conflict(completed_kind,
                              source_path,
                              destination_parent,
                              operation->message);
        return;
    }

    // A mode change does not create or rename an entry. Re-selecting the
    // earlier pathname after its asynchronous completion can override a newer
    // user navigation/selection (including an explicitly revealed hidden file).
    if (completed_kind != Kind::Permissions && operation->changed &&
        self->current_location_is_path(operation->destination.parent_path())) {
        self->pending_selection_name_ = operation->destination.filename().string();
        self->select_pending_item();
    }

    if (self->status_label_ != nullptr) {
        gtk_label_set_text(GTK_LABEL(self->status_label_), operation->message.c_str());
    }

    if (!operation->ok() && operation->status != OperationStatus::Cancelled) {
        self->show_alert(operation->status == OperationStatus::VerificationFailure
                             ? "Operation completed with a verification problem"
                             : operation->status == OperationStatus::UnsupportedCapability
                                   ? "Operation not supported safely"
                             : "File operation failed",
                         operation->message);
    }
}

void ItemOperationController::set_busy(const bool busy)
{
    if (!mutation_guard_.alive()) return;
    busy_ = busy;
    if (busy) progress_view_.start(current_control_);
    else progress_view_.finish();
    update_action_state();
}

void ItemOperationController::update_action_state()
{
    if (!mutation_guard_.alive()) return;
    if (menu_button_ == nullptr) {
        return;
    }

    std::string path;
    std::string name;
    const bool has_selection = selected_source(path, name);
    const bool enabled = !busy_ && !chooser_busy_ && has_selection;
    gtk_widget_set_sensitive(menu_button_, enabled);
    if (rename_button_ != nullptr) {
        gtk_widget_set_sensitive(rename_button_, enabled);
    }
    if (copy_button_ != nullptr) {
        gtk_widget_set_sensitive(copy_button_, enabled);
    }
    if (move_button_ != nullptr) {
        gtk_widget_set_sensitive(move_button_, enabled);
    }
    if (permissions_button_ != nullptr) {
        gtk_widget_set_sensitive(permissions_button_, enabled);
    }
}

void ItemOperationController::select_pending_item()
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

void ItemOperationController::close_menu()
{
    if (!mutation_guard_.alive()) return;
    if (popover_ != nullptr) {
        gtk_popover_popdown(GTK_POPOVER(popover_));
    }
}

void ItemOperationController::show_alert(const char *title, const std::string &detail) const
{
    if (window_ == nullptr) {
        return;
    }
    GtkAlertDialog *alert = gtk_alert_dialog_new("%s", title);
    gtk_alert_dialog_set_detail(alert, detail.c_str());
    gtk_alert_dialog_show(alert, window_);
    g_object_unref(alert);
}

bool ItemOperationController::selected_source(std::string &path, std::string &name) const
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

bool ItemOperationController::current_location_is_path(const std::filesystem::path &path) const
{
    if (directory_list_ == nullptr || path.empty()) {
        return false;
    }
    GFile *current = gtk_directory_list_get_file(directory_list_);
    if (current == nullptr || !g_file_is_native(current)) {
        return false;
    }

    GFile *candidate = g_file_new_for_path(path.string().c_str());
    const bool same = g_file_equal(current, candidate) != FALSE;
    g_object_unref(candidate);
    return same;
}

} // namespace infiltrator::files
