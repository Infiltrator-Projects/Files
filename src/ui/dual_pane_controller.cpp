// SPDX-License-Identifier: GPL-3.0-or-later
#include "dual_pane_controller.hpp"
#include "clipboard_controller.hpp"

#include <string>
#include <vector>

namespace infiltrator::files {
namespace {
constexpr const char *kAttributes =
    "standard::name,standard::display-name,standard::type,standard::icon,standard::is-hidden";

GtkWidget *action(GtkWidget *parent, const char *label, const char *tip,
                  int transfer_kind, gpointer controller, GCallback handler)
{
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_widget_set_tooltip_text(button, tip);
    gtk_widget_add_css_class(button, "flat");
    g_object_set_data(G_OBJECT(button), "ifm-pane-transfer", GINT_TO_POINTER(transfer_kind));
    g_signal_connect(button, "clicked", handler, controller);
    gtk_box_append(GTK_BOX(parent), button);
    return button;
}
} // namespace

DualPaneController::DualPaneController(GtkWindow *window, GtkWidget *host,
                                     GtkDirectoryList *primary,
                                     GtkMultiSelection *primary_selection,
                                     GtkWidget *status_label)
    : window_(window), host_(host), primary_(primary),
      primary_selection_(primary_selection), status_label_(status_label)
{
    g_object_ref(primary_);
    g_object_ref(primary_selection_);
    secondary_ = gtk_directory_list_new(kAttributes, nullptr);
    gtk_directory_list_set_monitored(secondary_, TRUE);
    secondary_selection_ = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(secondary_)));
    gtk_single_selection_set_autoselect(secondary_selection_, FALSE);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);
    back_ = gtk_button_new_from_icon_name("go-previous-symbolic");
    gtk_widget_set_tooltip_text(back_, "Second pane: Back");
    g_signal_connect(back_, "clicked", G_CALLBACK(on_back), this);
    gtk_box_append(GTK_BOX(bar), back_);
    GtkWidget *up = gtk_button_new_from_icon_name("go-up-symbolic");
    gtk_widget_set_tooltip_text(up, "Second pane: Parent");
    g_signal_connect(up, "clicked", G_CALLBACK(on_up), this);
    gtk_box_append(GTK_BOX(bar), up);
    path_ = gtk_entry_new();
    gtk_widget_set_hexpand(path_, TRUE);
    gtk_widget_set_tooltip_text(path_, "Second pane location (path or URI)");
    g_signal_connect(path_, "activate", G_CALLBACK(on_go), this);
    gtk_box_append(GTK_BOX(bar), path_);
    gtk_box_append(GTK_BOX(host_), bar);

    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(setup_row), this);
    g_signal_connect(factory, "bind", G_CALLBACK(bind_row), this);
    GtkWidget *view = gtk_list_view_new(
        GTK_SELECTION_MODEL(g_object_ref(secondary_selection_)), factory);
    gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(view), FALSE);
    g_signal_connect(view, "activate", G_CALLBACK(on_activate), this);
    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view);
    gtk_box_append(GTK_BOX(host_), scroller);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_margin_start(toolbar, 8);
    gtk_widget_set_margin_end(toolbar, 8);
    gtk_widget_set_margin_top(toolbar, 5);
    gtk_widget_set_margin_bottom(toolbar, 5);
    action(toolbar, "Copy →", "Copy selected items in the main pane into the second pane", 1, this, G_CALLBACK(on_transfer));
    action(toolbar, "Move →", "Move selected items in the main pane into the second pane", 2, this, G_CALLBACK(on_transfer));
    action(toolbar, "← Copy", "Copy selected item in the second pane into the main pane", 3, this, G_CALLBACK(on_transfer));
    action(toolbar, "← Move", "Move selected item in the second pane into the main pane", 4, this, G_CALLBACK(on_transfer));
    gtk_box_append(GTK_BOX(host_), toolbar);
    feedback_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(feedback_), 0);
    gtk_label_set_wrap(GTK_LABEL(feedback_), TRUE);
    gtk_widget_set_margin_start(feedback_, 8);
    gtk_widget_set_margin_bottom(feedback_, 6);
    gtk_box_append(GTK_BOX(host_), feedback_);
    g_signal_connect(secondary_, "notify::loading", G_CALLBACK(on_loading), this);
    g_signal_connect(secondary_, "notify::error", G_CALLBACK(on_loading), this);
    g_object_set_data(G_OBJECT(window_), "ifm-dual-pane-controller", this);
    gtk_widget_set_visible(host_, FALSE);
    gtk_widget_set_sensitive(back_, FALSE);
    g_object_weak_ref(G_OBJECT(window_), &DualPaneController::on_window_finalized, this);
}

DualPaneController::~DualPaneController()
{
    if (secondary_ != nullptr) g_signal_handlers_disconnect_by_data(secondary_, this);
    g_clear_object(&secondary_selection_);
    g_clear_object(&secondary_);
    g_clear_object(&primary_selection_);
    g_clear_object(&primary_);
}

void DualPaneController::on_window_finalized(gpointer data, GObject *)
{
    delete static_cast<DualPaneController *>(data);
}

bool DualPaneController::visible() const noexcept
{
    return host_ != nullptr && gtk_widget_get_visible(host_);
}

void DualPaneController::toggle()
{
    if (visible()) {
        gtk_widget_set_visible(host_, FALSE);
        gtk_directory_list_set_file(secondary_, nullptr);
        history_.clear();
        return;
    }
    GFile *origin = gtk_directory_list_get_file(primary_);
    if (origin == nullptr) {
        status("No current folder is available for the second pane.");
        return;
    }
    navigate(origin);
    gtk_widget_set_visible(host_, TRUE);
}

void DualPaneController::navigate(GFile *file)
{
    if (file == nullptr) return;
    char *uri = g_file_get_uri(file);
    if (uri == nullptr) return;
    GFile *old = gtk_directory_list_get_file(secondary_);
    if (old != nullptr && g_file_equal(old, file)) {
        g_free(uri);
        return;
    }
    if (old != nullptr) {
        char *prior = g_file_get_uri(old);
        if (prior != nullptr && history_.size() < 128) history_.emplace_back(prior);
        g_free(prior);
    }
    gtk_directory_list_set_file(secondary_, file);
    char *display = g_file_get_parse_name(file);
    gtk_editable_set_text(GTK_EDITABLE(path_), display != nullptr ? display : uri);
    g_free(display);
    g_free(uri);
    gtk_widget_set_sensitive(back_, !history_.empty());
    gtk_label_set_text(GTK_LABEL(feedback_), "");
}

void DualPaneController::setup_row(GtkSignalListItemFactory *, GtkListItem *item, gpointer)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(row, 10);
    gtk_widget_set_margin_end(row, 8);
    gtk_widget_set_margin_top(row, 5);
    gtk_widget_set_margin_bottom(row, 5);
    gtk_box_append(GTK_BOX(row), gtk_image_new_from_icon_name("text-x-generic-symbolic"));
    GtkWidget *label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(row), label);
    gtk_list_item_set_child(item, row);
}

void DualPaneController::bind_row(GtkSignalListItemFactory *, GtkListItem *item, gpointer)
{
    auto *info = G_FILE_INFO(gtk_list_item_get_item(item));
    GtkWidget *row = gtk_list_item_get_child(item);
    if (info == nullptr || row == nullptr) return;
    GtkWidget *icon = gtk_widget_get_first_child(row);
    GtkWidget *label = gtk_widget_get_next_sibling(icon);
    const bool folder = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    gtk_image_set_from_icon_name(GTK_IMAGE(icon),
                                 folder ? "folder-symbolic" : "text-x-generic-symbolic");
    const char *name = g_file_info_get_display_name(info);
    gtk_label_set_text(GTK_LABEL(label), name != nullptr ? name : "Unnamed item");
}

void DualPaneController::on_activate(GtkListView *, guint position, gpointer data)
{
    auto *self = static_cast<DualPaneController *>(data);
    GFile *parent = gtk_directory_list_get_file(self->secondary_);
    auto *info = G_FILE_INFO(g_list_model_get_item(G_LIST_MODEL(self->secondary_), position));
    if (parent == nullptr || info == nullptr) { g_clear_object(&info); return; }
    const char *name = g_file_info_get_name(info);
    if (name != nullptr) {
        GFile *child = g_file_get_child(parent, name);
        if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY)
            self->navigate(child);
        else {
            char *uri = g_file_get_uri(child);
            if (uri != nullptr) {
                g_app_info_launch_default_for_uri_async(uri, nullptr, nullptr, nullptr, nullptr);
                g_free(uri);
            }
        }
        g_object_unref(child);
    }
    g_object_unref(info);
}

void DualPaneController::on_go(GtkEntry *entry, gpointer data)
{
    auto *self = static_cast<DualPaneController *>(data);
    const char *value = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (value == nullptr || *value == '\0') return;
    GFile *file = g_file_parse_name(value);
    self->navigate(file);
    g_object_unref(file);
}

void DualPaneController::on_up(GtkButton *, gpointer data)
{
    auto *self = static_cast<DualPaneController *>(data);
    GFile *file = gtk_directory_list_get_file(self->secondary_);
    if (file == nullptr) return;
    GFile *parent = g_file_get_parent(file);
    if (parent != nullptr) { self->navigate(parent); g_object_unref(parent); }
}

void DualPaneController::on_back(GtkButton *, gpointer data)
{
    auto *self = static_cast<DualPaneController *>(data);
    if (self->history_.empty()) return;
    const std::string uri = self->history_.back();
    self->history_.pop_back();
    GFile *file = g_file_new_for_uri(uri.c_str());
    gtk_directory_list_set_file(self->secondary_, file);
    char *label = g_file_get_parse_name(file);
    gtk_editable_set_text(GTK_EDITABLE(self->path_), label != nullptr ? label : uri.c_str());
    g_free(label);
    g_object_unref(file);
    gtk_widget_set_sensitive(self->back_, !self->history_.empty());
}

void DualPaneController::on_loading(GObject *, GParamSpec *, gpointer data)
{
    auto *self = static_cast<DualPaneController *>(data);
    if (gtk_directory_list_is_loading(self->secondary_)) {
        gtk_label_set_text(GTK_LABEL(self->feedback_), "Loading second pane…");
        return;
    }
    const GError *error = gtk_directory_list_get_error(self->secondary_);
    gtk_label_set_text(GTK_LABEL(self->feedback_),
                       error != nullptr ? error->message : "");
}

std::vector<std::string> DualPaneController::selected_sources(bool from_primary) const
{
    std::vector<std::string> result;
    GtkDirectoryList *directory = from_primary ? primary_ : secondary_;
    GFile *parent = gtk_directory_list_get_file(directory);
    if (parent == nullptr || !g_file_is_native(parent)) return result;
    GListModel *model = from_primary ? gtk_multi_selection_get_model(primary_selection_)
                                    : G_LIST_MODEL(secondary_);
    if (model == nullptr) return result;
    if (!from_primary) {
        auto *info = G_FILE_INFO(gtk_single_selection_get_selected_item(secondary_selection_));
        if (info != nullptr && g_file_info_get_name(info) != nullptr) {
            GFile *child = g_file_get_child(parent, g_file_info_get_name(info));
            char *path = g_file_get_path(child);
            if (path != nullptr) { result.emplace_back(path); g_free(path); }
            g_object_unref(child);
        }
        return result;
    }
    GtkBitset *bits = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(primary_selection_));
    if (gtk_bitset_get_size(bits) > 256U) {
        gtk_bitset_unref(bits);
        return result;
    }
    GtkBitsetIter iter;
    guint index = 0;
    for (gboolean valid = gtk_bitset_iter_init_first(&iter, bits, &index);
         valid; valid = gtk_bitset_iter_next(&iter, &index)) {
        auto *info = G_FILE_INFO(g_list_model_get_item(model, index));
        if (info != nullptr && g_file_info_get_name(info) != nullptr) {
            GFile *child = g_file_get_child(parent, g_file_info_get_name(info));
            char *path = g_file_get_path(child);
            if (path != nullptr) { result.emplace_back(path); g_free(path); }
            g_object_unref(child);
        }
        g_clear_object(&info);
    }
    gtk_bitset_unref(bits);
    return result;
}

void DualPaneController::status(const char *message) const
{
    if (feedback_ != nullptr) gtk_label_set_text(GTK_LABEL(feedback_), message);
    if (status_label_ != nullptr) gtk_label_set_text(GTK_LABEL(status_label_), message);
}

void DualPaneController::transfer(bool to_secondary, bool move)
{
    GFile *destination = gtk_directory_list_get_file(to_secondary ? secondary_ : primary_);
    if (destination == nullptr || !g_file_is_native(destination)) {
        status("Cross-pane transfers require a native destination.");
        return;
    }
    char *path = g_file_get_path(destination);
    auto sources = selected_sources(to_secondary);
    if (path == nullptr || sources.empty()) {
        status("Select local source items in the opposite pane first.");
        g_free(path);
        return;
    }
    auto *controller = static_cast<ClipboardController *>(
        g_object_get_data(G_OBJECT(window_), "ifm-clipboard-controller"));
    if (controller == nullptr || !controller->transfer_from_drop(sources, path, move))
        status("Another transfer is active, or the window is no longer available.");
    g_free(path);
}

void DualPaneController::on_transfer(GtkButton *button, gpointer data)
{
    auto *self = static_cast<DualPaneController *>(data);
    const int mode = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(button), "ifm-pane-transfer"));
    if (mode >= 1 && mode <= 4)
        self->transfer(mode <= 2, (mode % 2) == 0);
}

} // namespace infiltrator::files
