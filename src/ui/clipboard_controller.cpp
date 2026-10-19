// SPDX-License-Identifier: GPL-3.0-or-later
#include "clipboard_controller.hpp"

#include <algorithm>
#include <filesystem>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace infiltrator::files {

namespace {

struct ClipboardReadData {
    ClipboardController *controller{nullptr};
    GtkWindow *held_window{nullptr};
    GInputStream *stream{nullptr};
    std::string destination;
    guint64 generation{0U};
    bool special{false};
    std::vector<char> bytes = std::vector<char>(256U * 1024U + 1U);
    ~ClipboardReadData() { g_clear_object(&stream); if (held_window) g_object_unref(held_window); }
};

struct ClipboardTaskData {
    int mode{0};
    int policy{0};
    std::vector<std::string> sources;
    std::string destination_parent;
};

bool tooltip_equals(GtkWidget *widget, const char *expected)
{
    if (widget == nullptr || expected == nullptr) {
        return false;
    }
    return g_strcmp0(gtk_widget_get_tooltip_text(widget), expected) == 0;
}

GtkWidget *find_widget_by_tooltip(GtkWidget *root, const char *tooltip)
{
    if (root == nullptr) {
        return nullptr;
    }
    if (tooltip_equals(root, tooltip)) {
        return root;
    }
    for (GtkWidget *child = gtk_widget_get_first_child(root);
         child != nullptr;
         child = gtk_widget_get_next_sibling(child)) {
        if (GtkWidget *match = find_widget_by_tooltip(child, tooltip); match != nullptr) {
            return match;
        }
    }
    return nullptr;
}

bool path_present(const std::string &path)
{
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    return !error && status.type() != std::filesystem::file_type::not_found;
}

} // namespace

ClipboardController::ClipboardController(GtkWindow *window,
                                         GtkDirectoryList *directory_list,
                                         GtkMultiSelection *selection,
                                         GtkWidget *status_label)
    : window_(window),
      directory_list_(directory_list),
      selection_(selection),
      status_label_(status_label)
{
    if (window_ == nullptr || directory_list_ == nullptr || selection_ == nullptr) {
        return;
    }

    g_object_ref(directory_list_);
    g_object_ref(selection_);
    GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(window_));
    if (display != nullptr) {
        clipboard_ = gdk_display_get_clipboard(display);
        if (clipboard_ != nullptr) {
            g_object_ref(clipboard_);
            clipboard_changed_handler_ = g_signal_connect(
                clipboard_, "changed", G_CALLBACK(on_clipboard_changed), this);
        }
    }
    g_object_set_data(G_OBJECT(window_), "ifm-clipboard-controller", this);

    install_menu_actions();
    install_keyboard_shortcuts();

    selection_handler_ = g_signal_connect(
        selection_, "selection-changed",
        G_CALLBACK(&ClipboardController::on_selection_changed), this);
    location_handler_ = g_signal_connect(
        directory_list_, "notify::file",
        G_CALLBACK(&ClipboardController::on_location_changed), this);

    update_action_state();
    // Destroy precedes finalisation. Dismiss child action pointers before GTK
    // disposes the buttons and before pending GIO work can emit late signals.
    g_signal_connect(window_, "destroy",
                     G_CALLBACK(&ClipboardController::on_window_destroyed), this);
    g_object_weak_ref(G_OBJECT(window_), &ClipboardController::on_window_finalized, this);
}

ClipboardController::~ClipboardController()
{
    if (clipboard_ != nullptr) {
        if (clipboard_changed_handler_ != 0U)
            g_signal_handler_disconnect(clipboard_, clipboard_changed_handler_);
        g_clear_object(&owned_content_);
        g_clear_object(&clipboard_);
    }
    if (window_ != nullptr)
        g_object_set_data(G_OBJECT(window_), "ifm-clipboard-controller", nullptr);
    if (selection_ != nullptr) {
        if (selection_handler_ != 0U) {
            g_signal_handler_disconnect(selection_, selection_handler_);
        }
        g_object_unref(selection_);
        selection_ = nullptr;
    }
    if (directory_list_ != nullptr) {
        if (location_handler_ != 0U) {
            g_signal_handler_disconnect(directory_list_, location_handler_);
        }
        g_object_unref(directory_list_);
        directory_list_ = nullptr;
    }
}

void ClipboardController::on_window_destroyed(GtkWidget *, gpointer user_data)
{
    auto *self = static_cast<ClipboardController *>(user_data);
    self->shutting_down_ = true;
    ++self->clipboard_generation_;
    if (self->clipboard_ != nullptr && self->clipboard_changed_handler_ != 0U) {
        g_signal_handler_disconnect(self->clipboard_, self->clipboard_changed_handler_);
        self->clipboard_changed_handler_ = 0U;
    }
    // The widgets are owned by the native window, not by the controller.
    self->copy_button_ = nullptr;
    self->cut_button_ = nullptr;
    self->paste_button_ = nullptr;
    self->status_label_ = nullptr;
}

void ClipboardController::on_window_finalized(gpointer user_data, GObject *where_object_was)
{
    (void)where_object_was;
    auto *self = static_cast<ClipboardController *>(user_data);
    self->window_ = nullptr;
    delete self;
}

void ClipboardController::install_menu_actions()
{
    GtkWidget *titlebar = gtk_window_get_titlebar(window_);
    GtkWidget *menu_button = find_widget_by_tooltip(titlebar, "File actions");
    if (!GTK_IS_MENU_BUTTON(menu_button)) {
        return;
    }

    GtkWidget *popover = GTK_WIDGET(
        gtk_menu_button_get_popover(GTK_MENU_BUTTON(menu_button)));
    if (!GTK_IS_POPOVER(popover)) {
        return;
    }

    GtkWidget *box = gtk_popover_get_child(GTK_POPOVER(popover));
    if (!GTK_IS_BOX(box)) {
        return;
    }

    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(separator, 3);
    gtk_widget_set_margin_bottom(separator, 3);
    gtk_box_append(GTK_BOX(box), separator);

    copy_button_ = gtk_button_new_with_label("Copy");
    cut_button_ = gtk_button_new_with_label("Cut");
    paste_button_ = gtk_button_new_with_label("Paste");

    for (GtkWidget *button : {copy_button_, cut_button_, paste_button_}) {
        gtk_widget_add_css_class(button, "flat");
        gtk_widget_set_halign(button, GTK_ALIGN_FILL);
        gtk_box_append(GTK_BOX(box), button);
    }

    gtk_widget_set_tooltip_text(copy_button_, "Copy selected items to the clipboard (Ctrl+C)");
    gtk_widget_set_tooltip_text(cut_button_, "Cut selected items to the clipboard (Ctrl+X)");
    gtk_widget_set_tooltip_text(paste_button_, "Paste clipboard items into this folder (Ctrl+V)");

    g_signal_connect(copy_button_, "clicked",
                     G_CALLBACK(&ClipboardController::on_copy_clicked), this);
    g_signal_connect(cut_button_, "clicked",
                     G_CALLBACK(&ClipboardController::on_cut_clicked), this);
    g_signal_connect(paste_button_, "clicked",
                     G_CALLBACK(&ClipboardController::on_paste_clicked), this);
}

void ClipboardController::install_keyboard_shortcuts()
{
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed",
                     G_CALLBACK(&ClipboardController::on_key_pressed), this);
    gtk_widget_add_controller(GTK_WIDGET(window_), keys);
}

void ClipboardController::on_copy_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    static_cast<ClipboardController *>(user_data)->capture_selection(Mode::Copy);
}

void ClipboardController::on_cut_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    static_cast<ClipboardController *>(user_data)->capture_selection(Mode::Cut);
}

void ClipboardController::on_paste_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    static_cast<ClipboardController *>(user_data)->paste();
}

gboolean ClipboardController::on_key_pressed(GtkEventControllerKey *controller,
                                             const guint keyval,
                                             const guint keycode,
                                             const GdkModifierType state,
                                             gpointer user_data)
{
    (void)controller;
    (void)keycode;
    auto *self = static_cast<ClipboardController *>(user_data);

    if ((state & GDK_CONTROL_MASK) == 0 || (state & GDK_SHIFT_MASK) != 0 ||
        self->busy_ ||
        self->reading_external_clipboard_) {
        return FALSE;
    }

    GtkWidget *focus = gtk_window_get_focus(self->window_);
    if (GTK_IS_EDITABLE(focus)) {
        return FALSE;
    }

    if (keyval == GDK_KEY_c || keyval == GDK_KEY_C) {
        self->capture_selection(Mode::Copy);
        return TRUE;
    }
    if (keyval == GDK_KEY_x || keyval == GDK_KEY_X) {
        self->capture_selection(Mode::Cut);
        return TRUE;
    }
    if (keyval == GDK_KEY_v || keyval == GDK_KEY_V) {
        self->paste();
        return TRUE;
    }
    return FALSE;
}

void ClipboardController::on_selection_changed(GtkSelectionModel *model,
                                               const guint position,
                                               const guint n_items,
                                               gpointer user_data)
{
    (void)model;
    (void)position;
    (void)n_items;
    static_cast<ClipboardController *>(user_data)->update_action_state();
}

void ClipboardController::on_location_changed(GObject *object,
                                              GParamSpec *pspec,
                                              gpointer user_data)
{
    (void)object;
    (void)pspec;
    static_cast<ClipboardController *>(user_data)->update_action_state();
}

void ClipboardController::on_clipboard_changed(GdkClipboard *clipboard, gpointer data)
{
    auto *self = static_cast<ClipboardController *>(data);
    if (self->shutting_down_) return;
    if (gdk_clipboard_get_content(clipboard) == self->owned_content_) return;
    ++self->clipboard_generation_;
    self->mode_ = Mode::None;
    self->sources_.clear();
    g_clear_object(&self->owned_content_);
    self->update_action_state();
}

void ClipboardController::capture_selection(const Mode mode)
{
    if (shutting_down_ || busy_ || reading_external_clipboard_ || mode == Mode::None) {
        return;
    }

    std::vector<std::string> sources = selected_sources();
    if (sources.empty()) {
        set_status("Select one or more local items to copy or cut.");
        return;
    }

    mode_ = mode;
    sources_ = std::move(sources);
    publish_clipboard_text();

    std::string message = mode == Mode::Copy ? "Copied " : "Cut ";
    message += std::to_string(sources_.size());
    message += sources_.size() == 1U ? " item to the clipboard." : " items to the clipboard.";
    set_status(message);
    update_action_state();
}

void ClipboardController::paste()
{
    if (shutting_down_ || busy_ || reading_external_clipboard_) {
        return;
    }

    const std::string destination = current_destination();
    if (destination.empty()) {
        set_status("Paste currently requires a local destination folder.");
        return;
    }

    if (clipboard_ != nullptr && owned_content_ != nullptr &&
        gdk_clipboard_get_content(clipboard_) != owned_content_) {
        mode_ = Mode::None;
        sources_.clear();
        g_clear_object(&owned_content_);
    }
    if (!sources_.empty() && mode_ != Mode::None && owned_content_ != nullptr) {
        if (mode_ == Mode::Cut) {
            const bool all_already_here = std::all_of(
                sources_.begin(), sources_.end(),
                [&destination](const std::string &source) {
                    return std::filesystem::path(source).parent_path() ==
                           std::filesystem::path(destination);
                });
            if (all_already_here) {
                set_status("The cut items are already in this folder.");
                return;
            }
        }
        paste_sources(mode_, sources_, destination, BatchConflictPolicy::Fail);
        return;
    }

    GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(window_));
    GdkClipboard *clipboard = display != nullptr ? gdk_display_get_clipboard(display) : nullptr;
    if (clipboard == nullptr) {
        set_status("The system clipboard is unavailable.");
        return;
    }

    reading_external_clipboard_ = true;
    clipboard_requested_destination_ = destination;
    clipboard_read_generation_ = clipboard_generation_;
    update_action_state();
    set_status("Reading file references from the clipboard…");
    g_object_ref(window_);
    const GdkContentFormats *formats = gdk_clipboard_get_formats(clipboard);
    const bool structured = formats != nullptr && (
        gdk_content_formats_contain_mime_type(formats, "x-special/gnome-copied-files") ||
        gdk_content_formats_contain_mime_type(formats, "text/uri-list"));
    if (structured) {
        const char *types[] = {"x-special/gnome-copied-files", "text/uri-list", nullptr};
        gdk_clipboard_read_async(clipboard, types, G_PRIORITY_DEFAULT, nullptr,
                                 &ClipboardController::on_clipboard_stream_open, this);
    } else {
        gdk_clipboard_read_text_async(clipboard, nullptr,
                                      &ClipboardController::on_clipboard_text_read, this);
    }
}


void ClipboardController::on_clipboard_stream_open(GObject *source_object,
                                                    GAsyncResult *result, gpointer user_data)
{
    auto *self = static_cast<ClipboardController *>(user_data);
    const char *mime = nullptr;
    GError *error = nullptr;
    GInputStream *stream = gdk_clipboard_read_finish(GDK_CLIPBOARD(source_object),
                                                      result, &mime, &error);
    if (self->shutting_down_ || stream == nullptr || error != nullptr) {
        self->reading_external_clipboard_ = false;
        self->update_action_state();
        self->set_status("Could not read file references from the clipboard.");
        g_clear_error(&error);
        if (stream != nullptr) g_object_unref(stream);
        g_object_unref(self->window_);
        return;
    }
    auto *read = new ClipboardReadData();
    read->controller = self;
    // Transfer the one window reference captured in paste() to this request.
    read->held_window = self->window_;
    read->stream = stream;
    read->generation = self->clipboard_read_generation_;
    read->special = g_strcmp0(mime, "x-special/gnome-copied-files") == 0;
    read->destination = self->clipboard_requested_destination_;
    g_input_stream_read_all_async(stream, read->bytes.data(), read->bytes.size(),
                                  G_PRIORITY_DEFAULT, nullptr,
                                  &ClipboardController::on_clipboard_stream_read, read);
}
void ClipboardController::on_clipboard_stream_read(GObject *source_object,
                                                    GAsyncResult *result, gpointer user_data)
{
    std::unique_ptr<ClipboardReadData> read(static_cast<ClipboardReadData *>(user_data));
    auto *self = read->controller;
    gsize n = 0;
    GError *error = nullptr;
    const bool ok = g_input_stream_read_all_finish(G_INPUT_STREAM(source_object),
                                                    result, &n, &error);
    self->reading_external_clipboard_ = false;
    if (self->shutting_down_) {
        g_clear_error(&error);
        return;
    }
    self->update_action_state();
    if (!ok || error != nullptr || n == read->bytes.size() ||
        read->generation != self->clipboard_generation_) {
        self->set_status("Clipboard changed, could not be read, or contains too many references.");
        g_clear_error(&error);
        return;
    }
    read->bytes[n] = '\0';
    if (memchr(read->bytes.data(), '\0', n) != nullptr) {
        self->set_status("The clipboard contains invalid file reference data.");
        return;
    }
    std::string input(read->bytes.data(), n);
    const bool cut = read->special && input.rfind("cut\n", 0U) == 0U;
    const auto sources = self->parse_local_file_references(input.c_str());
    if (sources.empty()) {
        self->set_status("The clipboard contains no accessible local file references.");
        return;
    }
    if (read->destination.empty() || read->destination != self->current_destination()) {
        self->set_status("The destination changed while reading the clipboard.");
        return;
    }
    self->paste_sources(cut ? Mode::Cut : Mode::Copy, sources, read->destination,
                        BatchConflictPolicy::Fail);
}

void ClipboardController::on_clipboard_text_read(GObject *source_object,
                                                 GAsyncResult *result,
                                                 gpointer user_data)
{
    auto *self = static_cast<ClipboardController *>(user_data);
    GtkWindow *held_window = self->window_;

    GError *error = nullptr;
    char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source_object), result, &error);
    self->reading_external_clipboard_ = false;
    // The outstanding clipboard request owns a window reference, but Destroy
    // has already revoked authority to start another filesystem mutation.
    if (self->shutting_down_) {
        g_free(text);
        g_clear_error(&error);
        g_object_unref(held_window);
        return;
    }
    self->update_action_state();

    if (self->clipboard_read_generation_ != self->clipboard_generation_) {
        g_free(text);
        g_clear_error(&error);
        self->set_status("Clipboard contents changed before Paste.");
        g_object_unref(held_window);
        return;
    }
    if (error != nullptr) {
        self->set_status("The clipboard does not contain readable local file references.");
        g_error_free(error);
        g_object_unref(held_window);
        return;
    }

    const std::vector<std::string> sources = self->parse_local_file_references(text);
    g_free(text);
    if (sources.empty()) {
        self->set_status("The clipboard does not contain local file references that Files can paste.");
        g_object_unref(held_window);
        return;
    }

    const std::string destination = self->clipboard_requested_destination_;
    if (destination.empty() || destination != self->current_destination()) {
        self->set_status("The destination changed while reading the clipboard.");
        g_object_unref(held_window);
        return;
    }

    self->paste_sources(Mode::Copy, sources, destination, BatchConflictPolicy::Fail);
    g_object_unref(held_window);
}

bool ClipboardController::transfer_from_drop(const std::vector<std::string> &sources,
                                             const std::string &destination)
{
    if (busy_ || reading_external_clipboard_ || window_ == nullptr ||
        sources.empty() || destination.empty()) return false;
    paste_sources(Mode::Copy, sources, destination, BatchConflictPolicy::Fail);
    return true;
}

void ClipboardController::paste_sources(const Mode mode,
                                        const std::vector<std::string> &sources,
                                        const std::string &destination_parent,
                                        const BatchConflictPolicy policy)
{
    if (busy_ || sources.empty() || destination_parent.empty() || mode == Mode::None ||
        window_ == nullptr) {
        return;
    }

    auto *data = new ClipboardTaskData{
        static_cast<int>(mode),
        static_cast<int>(policy),
        sources,
        destination_parent,
    };

    // GtkDirectoryList's native monitor queries changed files asynchronously.
    // During our own filesystem mutation, those monitor callbacks can still be
    // outstanding when the window is closed. Stop the native monitor before
    // the worker starts and reload the current listing on completion; ordinary
    // navigation/enumeration remains available throughout the transfer.
    restore_directory_monitor_ = directory_list_ != nullptr &&
        gtk_directory_list_get_monitored(directory_list_);
    if (restore_directory_monitor_)
        gtk_directory_list_set_monitored(directory_list_, FALSE);

    busy_ = true;
    update_action_state();
    set_status(mode == Mode::Cut ? "Moving clipboard items…" : "Copying clipboard items…");

    GTask *task = g_task_new(G_OBJECT(window_), nullptr,
                             &ClipboardController::on_operation_finished, this);
    g_task_set_task_data(task, data, [](gpointer value) {
        delete static_cast<ClipboardTaskData *>(value);
    });
    g_task_run_in_thread(task, &ClipboardController::on_operation_thread);
    g_object_unref(task);
}

void ClipboardController::on_operation_thread(GTask *task,
                                              gpointer source_object,
                                              gpointer task_data,
                                              GCancellable *cancellable)
{
    (void)source_object;
    (void)cancellable;
    const auto *data = static_cast<const ClipboardTaskData *>(task_data);

    std::vector<std::filesystem::path> sources;
    sources.reserve(data->sources.size());
    for (const std::string &source : data->sources) {
        sources.emplace_back(source);
    }

    const auto mode = static_cast<Mode>(data->mode);
    const BatchTransferKind kind = mode == Mode::Cut
        ? BatchTransferKind::Move
        : BatchTransferKind::Copy;

    BatchTransferEngine engine;
    auto *operation = new OperationResult(engine.execute(
        sources,
        data->destination_parent,
        kind,
        static_cast<BatchConflictPolicy>(data->policy)));
    g_task_return_pointer(task, operation, [](gpointer value) {
        delete static_cast<OperationResult *>(value);
    });
}

void ClipboardController::on_operation_finished(GObject *source_object,
                                                GAsyncResult *result,
                                                gpointer user_data)
{
    (void)source_object;
    auto *self = static_cast<ClipboardController *>(user_data);
    GTask *task = G_TASK(result);
    const auto *data = static_cast<const ClipboardTaskData *>(g_task_get_task_data(task));

    GError *error = nullptr;
    std::unique_ptr<OperationResult> operation(
        static_cast<OperationResult *>(g_task_propagate_pointer(task, &error)));
    self->busy_ = false;
    if (self->restore_directory_monitor_) {
        self->restore_directory_monitor_ = false;
        if (!self->shutting_down_ && self->directory_list_ != nullptr)
            gtk_directory_list_set_monitored(self->directory_list_, TRUE);
    }
    // The GTask holds a reference to its GTK window, but an explicitly
    // destroyed window may already have disposed all child widgets. Consume
    // completion without accessing status/actions during shutdown.
    if (self->shutting_down_) {
        g_clear_error(&error);
        return;
    }
    self->update_action_state();

    if (error != nullptr) {
        self->set_status(error->message);
        self->show_alert("Paste failed", error->message);
        g_error_free(error);
        return;
    }
    if (!operation || data == nullptr) {
        self->show_alert("Paste failed", "The clipboard operation did not return a result.");
        return;
    }

    const auto mode = static_cast<Mode>(data->mode);
    const auto policy = static_cast<BatchConflictPolicy>(data->policy);
    if (operation->status == OperationStatus::DestinationConflict &&
        policy == BatchConflictPolicy::Fail) {
        self->set_status(operation->message);
        self->prompt_conflict(mode, data->sources, data->destination_parent, operation->message);
        return;
    }

    self->set_status(operation->message);
    if (operation->ok() && mode == Mode::Cut) {
        self->refresh_cut_state_after_paste(data->sources);
    }
    if (!operation->ok() && operation->status != OperationStatus::Cancelled) {
        self->show_alert(operation->status == OperationStatus::VerificationFailure
                             ? "Paste completed with a verification problem"
                             : operation->status == OperationStatus::UnsupportedCapability
                                   ? "Paste is not supported safely here"
                                   : "Paste failed",
                         operation->message);
    }
}

void ClipboardController::prompt_conflict(const Mode mode,
                                          const std::vector<std::string> &sources,
                                          const std::string &destination_parent,
                                          const std::string &detail)
{
    if (window_ == nullptr || sources.empty() || destination_parent.empty()) {
        return;
    }

    pending_conflict_ = PendingConflict{mode, sources, destination_parent};

    GtkAlertDialog *dialog = gtk_alert_dialog_new("Some clipboard items already exist here");
    const std::string explanation = detail +
        " Choose one policy for conflicting items in this paste.";
    gtk_alert_dialog_set_detail(dialog, explanation.c_str());
    const char *buttons[] = {"Cancel", "Skip Conflicts", "Keep Both", "Replace Conflicts", nullptr};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_default_button(dialog, 2);

    g_object_ref(window_);
    gtk_alert_dialog_choose(dialog,
                            window_,
                            nullptr,
                            &ClipboardController::on_conflict_chosen,
                            this);
    g_object_unref(dialog);
}

void ClipboardController::on_conflict_chosen(GObject *source_object,
                                             GAsyncResult *result,
                                             gpointer user_data)
{
    auto *self = static_cast<ClipboardController *>(user_data);
    GtkWindow *held_window = self->window_;

    GError *error = nullptr;
    const int choice = gtk_alert_dialog_choose_finish(
        GTK_ALERT_DIALOG(source_object), result, &error);

    PendingConflict pending = std::move(self->pending_conflict_);
    self->pending_conflict_ = {};

    if (error != nullptr) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            self->show_alert("Resolve paste conflicts", error->message);
        }
        g_error_free(error);
        g_object_unref(held_window);
        return;
    }

    if (choice == 1) {
        self->paste_sources(pending.mode, pending.sources,
                            pending.destination_parent, BatchConflictPolicy::Skip);
    } else if (choice == 2) {
        self->paste_sources(pending.mode, pending.sources,
                            pending.destination_parent, BatchConflictPolicy::KeepBoth);
    } else if (choice == 3) {
        self->paste_sources(pending.mode, pending.sources,
                            pending.destination_parent, BatchConflictPolicy::Replace);
    }

    g_object_unref(held_window);
}

void ClipboardController::publish_clipboard_text()
{
    if (clipboard_ == nullptr) return;

    std::string payload;
    for (const std::string &source : sources_) {
        GFile *file = g_file_new_for_path(source.c_str());
        char *uri = g_file_get_uri(file);
        g_object_unref(file);
        if (uri == nullptr) {
            continue;
        }
        if (!payload.empty()) {
            payload += '\n';
        }
        payload += uri;
        g_free(uri);
    }
    std::string gnome = (mode_ == Mode::Cut ? "cut\n" : "copy\n") + payload + "\n";
    std::string uri_list;
    for (const std::string &source : sources_) {
        GFile *file = g_file_new_for_path(source.c_str());
        char *uri = g_file_get_uri(file);
        if (uri != nullptr) { uri_list += uri; uri_list += "\r\n"; }
        g_free(uri);
        g_object_unref(file);
    }
    GBytes *gnome_bytes = g_bytes_new(gnome.data(), gnome.size());
    GBytes *uri_bytes = g_bytes_new(uri_list.data(), uri_list.size());
    GdkContentProvider *providers[] = {
        gdk_content_provider_new_for_bytes("x-special/gnome-copied-files", gnome_bytes),
        gdk_content_provider_new_for_bytes("text/uri-list", uri_bytes),
        gdk_content_provider_new_typed(G_TYPE_STRING, payload.c_str())
    };
    GdkContentProvider *provider = gdk_content_provider_new_union(providers, G_N_ELEMENTS(providers));
    // gdk_content_provider_new_union takes ownership of every provider.
    // Releasing them here would leave the union with dangling pointers.
    g_bytes_unref(gnome_bytes);
    g_bytes_unref(uri_bytes);
    GdkContentProvider *previous = owned_content_;
    owned_content_ = provider;
    if (!gdk_clipboard_set_content(clipboard_, provider)) {
        owned_content_ = previous;
        g_object_unref(provider);
        mode_ = Mode::None;
        sources_.clear();
        set_status("Unable to claim the system clipboard.");
    } else if (previous != nullptr) {
        g_object_unref(previous);
    }
}

void ClipboardController::refresh_cut_state_after_paste(
    const std::vector<std::string> &sources)
{
    if (mode_ != Mode::Cut || sources_ != sources) {
        return;
    }

    std::vector<std::string> remaining;
    for (const std::string &source : sources_) {
        if (path_present(source)) {
            remaining.push_back(source);
        }
    }

    sources_ = std::move(remaining);
    if (sources_.empty()) {
        mode_ = Mode::None;
    }
    publish_clipboard_text();
    update_action_state();
}

std::vector<std::string> ClipboardController::selected_sources() const
{
    std::vector<std::string> sources;
    if (directory_list_ == nullptr || selection_ == nullptr) {
        return sources;
    }

    GFile *parent = gtk_directory_list_get_file(directory_list_);
    if (parent == nullptr || !g_file_is_native(parent)) {
        return sources;
    }

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

std::string ClipboardController::current_destination() const
{
    if (directory_list_ == nullptr) {
        return {};
    }
    GFile *folder = gtk_directory_list_get_file(directory_list_);
    if (folder == nullptr || !g_file_is_native(folder)) {
        return {};
    }
    char *path = g_file_get_path(folder);
    if (path == nullptr) {
        return {};
    }
    std::string destination(path);
    g_free(path);
    return destination;
}

std::vector<std::string> ClipboardController::parse_local_file_references(
    const char *text) const
{
    std::vector<std::string> result;
    if (text == nullptr || text[0] == '\0') {
        return result;
    }

    if (strlen(text) > 256U * 1024U || !g_utf8_validate(text, -1, nullptr)) return result;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (result.size() >= 512U) break;
        if (line.empty() || line[0] == '#' || line == "copy" || line == "cut") {
            continue;
        }

        std::string path;
        if (line.rfind("file://", 0U) == 0U) {
            GFile *file = g_file_new_for_uri(line.c_str());
            if (g_file_is_native(file)) {
                char *native_path = g_file_get_path(file);
                if (native_path != nullptr) {
                    path = native_path;
                    g_free(native_path);
                }
            }
            g_object_unref(file);
        } else if (g_path_is_absolute(line.c_str())) {
            path = line;
        }

        if (!path.empty() && path_present(path) &&
            std::find(result.begin(), result.end(), path) == result.end()) {
            result.push_back(std::move(path));
        }
    }
    return result;
}

void ClipboardController::update_action_state()
{
    if (shutting_down_) return;
    GtkBitset *selected = selection_ != nullptr
        ? gtk_selection_model_get_selection(GTK_SELECTION_MODEL(selection_))
        : nullptr;
    const bool has_selection = selected != nullptr && gtk_bitset_get_size(selected) > 0U;
    if (selected != nullptr) {
        gtk_bitset_unref(selected);
    }

    const bool local_destination = !current_destination().empty();
    const bool available = !busy_ && !reading_external_clipboard_;

    if (copy_button_ != nullptr) {
        gtk_widget_set_sensitive(copy_button_, available && local_destination && has_selection);
    }
    if (cut_button_ != nullptr) {
        gtk_widget_set_sensitive(cut_button_, available && local_destination && has_selection);
    }
    if (paste_button_ != nullptr) {
        gtk_widget_set_sensitive(paste_button_, available && local_destination);
    }
}

void ClipboardController::set_status(const std::string &message) const
{
    if (status_label_ != nullptr) {
        gtk_label_set_text(GTK_LABEL(status_label_), message.c_str());
    }
}

void ClipboardController::show_alert(const char *title, const std::string &detail) const
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
