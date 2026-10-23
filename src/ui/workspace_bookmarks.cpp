// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_manager_window.hpp"
#include "presentation_store.hpp"
#include "theme_controller.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace infiltrator::files {
namespace {
bool valid_label(const std::string &label)
{
    if (label.size() > 512U || !g_utf8_validate(label.c_str(), static_cast<gssize>(label.size()), nullptr) ||
        g_utf8_strlen(label.c_str(), -1) > 128) return false;
    return std::none_of(label.begin(), label.end(), [](unsigned char c) { return c < 32U || c == 127U; });
}
bool valid_bookmark_uri(const std::string &uri)
{
    if (uri.empty() || uri.size() > 16384U) return false;
    char *scheme = g_uri_parse_scheme(uri.c_str());
    const bool valid = scheme != nullptr;
    g_free(scheme); return valid;
}
}
void FileManagerWindow::load_bookmarks()
{
    std::string contents, error;
    auto status = presentation_store::read("bookmarks.ini", contents, error);
    std::vector<std::pair<std::string, std::string>> parsed;
    if (status == presentation_store::ReadStatus::Missing) {
        // Import the earlier URI-per-line format without deleting that file.
        status = presentation_store::read("bookmarks", contents, error);
        if (status == presentation_store::ReadStatus::Missing) return;
        if (status == presentation_store::ReadStatus::Loaded) {
            std::istringstream lines(contents);
            for (std::string uri; std::getline(lines, uri);) {
                if (uri.empty()) continue;
                if (!valid_bookmark_uri(uri) || parsed.size() == 128U) {
                    status = presentation_store::ReadStatus::Failed; break;
                }
                parsed.emplace_back(std::move(uri), "");
            }
        }
    } else if (status == presentation_store::ReadStatus::Loaded) {
        GKeyFile *keys = g_key_file_new();
        GError *parse_error = nullptr;
        bool valid = g_key_file_load_from_data(keys, contents.data(), contents.size(), G_KEY_FILE_NONE, &parse_error);
        int count = -1;
        if (valid) count = g_key_file_get_integer(keys, "Bookmarks", "count", &parse_error);
        valid = valid && parse_error == nullptr && count >= 0 && count <= 128 &&
            g_key_file_get_integer(keys, "Bookmarks", "version", nullptr) == 1;
        for (int i = 0; valid && i < count; ++i) {
            const std::string group = "Bookmark " + std::to_string(i);
            char *uri = g_key_file_get_string(keys, group.c_str(), "uri", nullptr);
            char *label = g_key_file_get_string(keys, group.c_str(), "label", nullptr);
            valid = uri != nullptr && label != nullptr && valid_bookmark_uri(uri) && valid_label(label);
            if (valid) parsed.emplace_back(uri, label);
            g_free(uri); g_free(label);
        }
        g_clear_error(&parse_error); g_key_file_unref(keys);
        if (!valid) status = presentation_store::ReadStatus::Failed;
    }
    if (status == presentation_store::ReadStatus::Failed) {
        bookmarks_writable_ = false;
        return;
    }
    for (const auto &[uri, label] : parsed) {
        if (std::none_of(bookmarks_.begin(), bookmarks_.end(), [&](const BookmarkRow &row) { return row.uri == uri; }))
            append_bookmark(uri, label);
    }
}
bool FileManagerWindow::save_bookmarks() const
{
    if (!bookmarks_writable_) {
        gtk_label_set_text(GTK_LABEL(status_label_), "Saved bookmarks could not be read. The existing bookmark file has been preserved.");
        return false;
    }
    GKeyFile *keys = g_key_file_new();
    g_key_file_set_integer(keys, "Bookmarks", "version", 1);
    g_key_file_set_integer(keys, "Bookmarks", "count", static_cast<int>(bookmarks_.size()));
    for (std::size_t i = 0; i < bookmarks_.size(); ++i) {
        const std::string group = "Bookmark " + std::to_string(i);
        g_key_file_set_string(keys, group.c_str(), "uri", bookmarks_[i].uri.c_str());
        g_key_file_set_string(keys, group.c_str(), "label", bookmarks_[i].label.c_str());
    }
    gsize bytes = 0;
    char *contents = g_key_file_to_data(keys, &bytes, nullptr);
    std::string error;
    const bool saved = contents != nullptr && presentation_store::write("bookmarks.ini", {contents, bytes}, error);
    g_free(contents); g_key_file_unref(keys);
    if (!saved) {
        gtk_label_set_text(GTK_LABEL(status_label_), error.c_str());
        return false;
    }
    // One durable authority; all other windows reload after a committed edit
    // before they can make another change from a stale bookmark ordering.
    auto *app = gtk_window_get_application(native_window());
    for (GList *item = app != nullptr ? gtk_application_get_windows(app) : nullptr;
         item != nullptr; item = item->next) {
        auto *other = static_cast<FileManagerWindow *>(g_object_get_data(G_OBJECT(item->data), "ifm-window-controller"));
        if (other != nullptr && other != this) other->reload_bookmarks();
    }
    return true;
}
void FileManagerWindow::reload_bookmarks()
{
    for (const auto &bookmark : bookmarks_)
        gtk_list_box_remove(GTK_LIST_BOX(sidebar_), bookmark.row);
    bookmarks_.clear(); bookmarks_writable_ = true;
    load_bookmarks(); update_bookmark_button();
}
void FileManagerWindow::append_bookmark(const std::string &uri, const std::string &custom_label)
{
    GFile *file = g_file_new_for_uri(uri.c_str());
    char *label = g_file_get_basename(file);
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *symbol = gtk_image_new_from_icon_name("starred-symbolic");
    gtk_box_append(GTK_BOX(content), symbol);
    GtkWidget *name = gtk_label_new(!custom_label.empty() ? custom_label.c_str() : (label != nullptr ? label : uri.c_str()));
    gtk_label_set_xalign(GTK_LABEL(name), 0.0F);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(name), 24);
    gtk_widget_set_tooltip_text(row, uri.c_str());
    gtk_widget_set_hexpand(name, TRUE);
    gtk_box_append(GTK_BOX(content), name);
    GtkWidget *options = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(options), "view-more-symbolic");
    gtk_widget_set_tooltip_text(options, "Bookmark options");
    GtkWidget *popup = gtk_popover_new();
    GtkWidget *items = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    for (const char *verb : {"Open in new tab", "Open in new window",
                              "Rename label…", "Move up", "Move down", "Remove"}) {
        GtkWidget *action = gtk_button_new_with_label(verb);
        gtk_widget_add_css_class(action, "flat");
        g_object_set_data(G_OBJECT(action), "ifm-bookmark-row", row);
        g_signal_connect(action, "clicked", G_CALLBACK(on_bookmark_action), this);
        gtk_box_append(GTK_BOX(items), action);
    }
    gtk_popover_set_child(GTK_POPOVER(popup), items);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(options), popup);
    gtk_box_append(GTK_BOX(content), options);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), content);
    g_object_set_data_full(G_OBJECT(row), "ifm-target", g_strdup(uri.c_str()), g_free);
    gtk_list_box_append(GTK_LIST_BOX(sidebar_), row);
    bookmarks_.push_back(BookmarkRow{uri, row, custom_label, name});
    install_bookmark_drag(row);
    update_sidebar_current_state();
    g_free(label);
    g_object_unref(file);
}
void FileManagerWindow::reorder_bookmarks()
{
    for (std::size_t i = 0; i < bookmarks_.size(); ++i) {
        GtkWidget *row = bookmarks_[i].row;
        const int desired = static_cast<int>(static_sidebar_row_count_ +
                                             mounted_place_rows_.size() + i);
        if (gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(row)) != desired) {
            g_object_ref(row);
            gtk_list_box_remove(GTK_LIST_BOX(sidebar_), row);
            gtk_list_box_insert(GTK_LIST_BOX(sidebar_), row, desired);
            g_object_unref(row);
        }
    }
}
void FileManagerWindow::remove_bookmark(const std::size_t index)
{
    if (index >= bookmarks_.size()) return;
    GtkWidget *row = bookmarks_[index].row;
    g_object_ref(row);
    gtk_list_box_remove(GTK_LIST_BOX(sidebar_), row);
    bookmarks_.erase(bookmarks_.begin() + static_cast<std::ptrdiff_t>(index));
    g_object_unref(row);
    reorder_bookmarks();
    save_bookmarks();
    update_bookmark_button();
}
void FileManagerWindow::update_bookmark_button()
{
    if (bookmark_button_ == nullptr || current_location_ == nullptr) return;
    char *uri = g_file_get_uri(current_location_);
    const bool pinned = uri != nullptr &&
        std::any_of(bookmarks_.begin(), bookmarks_.end(),
                    [uri](const BookmarkRow &bookmark){ return bookmark.uri == uri; });
    gtk_widget_set_tooltip_text(bookmark_button_,
                                pinned ? "Unpin current folder" : "Pin current folder");
    g_free(uri);
}
void FileManagerWindow::on_bookmark_toggle(GtkButton *, gpointer data)
{
    auto *self = static_cast<FileManagerWindow *>(data);
    if (!self->bookmarks_writable_) { self->save_bookmarks(); return; }
    if (self->current_location_ == nullptr) return;
    char *uri = g_file_get_uri(self->current_location_);
    if (uri == nullptr) return;
    const auto it = std::find_if(self->bookmarks_.begin(), self->bookmarks_.end(),
                                [uri](const BookmarkRow &entry){ return entry.uri == uri; });
    if (it != self->bookmarks_.end()) {
        self->remove_bookmark(static_cast<std::size_t>(it - self->bookmarks_.begin()));
    } else if (self->bookmarks_.size() < 128U) {
        self->append_bookmark(uri);
        self->save_bookmarks();
        self->update_bookmark_button();
    }
    g_free(uri);
}
void FileManagerWindow::on_bookmark_action(GtkButton *button, gpointer data)
{
    auto *self = static_cast<FileManagerWindow *>(data);
    if (!self->bookmarks_writable_) { self->save_bookmarks(); return; }
    GtkWidget *row = GTK_WIDGET(g_object_get_data(G_OBJECT(button), "ifm-bookmark-row"));
    const auto it = std::find_if(self->bookmarks_.begin(), self->bookmarks_.end(),
                               [row](const BookmarkRow &entry){ return entry.row == row; });
    if (it == self->bookmarks_.end()) return;
    const std::size_t index = static_cast<std::size_t>(it - self->bookmarks_.begin());
    const char *verb = gtk_button_get_label(button);
    if (g_strcmp0(verb, "Open in new tab") == 0) {
        GFile *target = g_file_new_for_uri(it->uri.c_str());
        self->add_tab(target);
        g_object_unref(target);
    } else if (g_strcmp0(verb, "Open in new window") == 0) {
        GtkApplication *app = gtk_window_get_application(self->native_window());
        if (app != nullptr) {
            GFile *target = g_file_new_for_uri(it->uri.c_str());
            auto *other = new FileManagerWindow(app);
            other->install_operation_surface();
            new ThemeController(other->native_window());
            other->present();
            other->open_location(target);
            g_object_unref(target);
        }
    } else if (g_strcmp0(verb, "Rename label…") == 0) {
        self->show_bookmark_label_dialog(it->uri);
    } else if (g_strcmp0(verb, "Remove") == 0) {
        self->remove_bookmark(index);
    } else if (g_strcmp0(verb, "Move up") == 0 && index > 0U) {
        std::swap(self->bookmarks_[index], self->bookmarks_[index - 1U]);
        self->reorder_bookmarks();
        self->save_bookmarks();
    } else if (g_strcmp0(verb, "Move down") == 0 &&
               index + 1U < self->bookmarks_.size()) {
        std::swap(self->bookmarks_[index], self->bookmarks_[index + 1U]);
        self->reorder_bookmarks();
        self->save_bookmarks();
    }
}

namespace {
struct BookmarkDrag {
    GWeakRef owner, row;
    BookmarkDrag(GtkWindow *window, GtkWidget *widget) {
        g_weak_ref_init(&owner, G_OBJECT(window)); g_weak_ref_init(&row, G_OBJECT(widget));
    }
    BookmarkDrag(const BookmarkDrag &other) {
        GObject *window = G_OBJECT(g_weak_ref_get(const_cast<GWeakRef *>(&other.owner)));
        GObject *widget = G_OBJECT(g_weak_ref_get(const_cast<GWeakRef *>(&other.row)));
        g_weak_ref_init(&owner, window); g_weak_ref_init(&row, widget);
        g_clear_object(&window); g_clear_object(&widget);
    }
    ~BookmarkDrag() { g_weak_ref_clear(&owner); g_weak_ref_clear(&row); }
};
struct BookmarkLabelDialog {
    GWeakRef owner;
    std::string uri;
    GtkWidget *dialog, *entry, *message;
    BookmarkLabelDialog(GtkWindow *window, const std::string &target) : uri(target) {
        g_weak_ref_init(&owner, G_OBJECT(window));
    }
    ~BookmarkLabelDialog() { g_weak_ref_clear(&owner); }
};
}

bool FileManagerWindow::rename_bookmark(const std::string &uri, const std::string &label)
{
    if (!bookmarks_writable_ || !valid_label(label)) return false;
    const auto found = std::find_if(bookmarks_.begin(), bookmarks_.end(), [&](const BookmarkRow &row) {
        return row.uri == uri;
    });
    if (found == bookmarks_.end()) return false;
    found->label = label;
    GFile *target = g_file_new_for_uri(uri.c_str());
    char *basename = g_file_get_basename(target);
    gtk_label_set_text(GTK_LABEL(found->caption), !label.empty() ? label.c_str() :
                       (basename != nullptr ? basename : uri.c_str()));
    g_free(basename); g_object_unref(target);
    return save_bookmarks();
}

void FileManagerWindow::show_bookmark_label_dialog(const std::string &uri)
{
    const auto found = std::find_if(bookmarks_.begin(), bookmarks_.end(), [&](const BookmarkRow &row) {
        return row.uri == uri;
    });
    if (found == bookmarks_.end()) return;
    auto *state = new BookmarkLabelDialog(native_window(), uri);
    state->dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(state->dialog), "Rename Bookmark");
    gtk_window_set_transient_for(GTK_WINDOW(state->dialog), native_window());
    gtk_window_set_destroy_with_parent(GTK_WINDOW(state->dialog), TRUE);
    gtk_window_set_modal(GTK_WINDOW(state->dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(state->dialog), 420, -1);
    new ThemeController(GTK_WINDOW(state->dialog));
    g_object_set_data_full(G_OBJECT(state->dialog), "ifm-bookmark-label", state,
        +[](gpointer data) { delete static_cast<BookmarkLabelDialog *>(data); });
    auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    for (auto setter : {gtk_widget_set_margin_top, gtk_widget_set_margin_bottom,
                       gtk_widget_set_margin_start, gtk_widget_set_margin_end}) setter(box, 18);
    gtk_window_set_child(GTK_WINDOW(state->dialog), box);
    auto *explanation = gtk_label_new("Choose a sidebar label. Leave it empty to use the folder name.");
    gtk_label_set_wrap(GTK_LABEL(explanation), TRUE);
    gtk_box_append(GTK_BOX(box), explanation);
    state->entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(state->entry), 128);
    gtk_editable_set_text(GTK_EDITABLE(state->entry), found->label.c_str());
    gtk_box_append(GTK_BOX(box), state->entry);
    state->message = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(state->message), TRUE);
    gtk_box_append(GTK_BOX(box), state->message);
    auto *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    auto *cancel = gtk_button_new_with_label("Cancel");
    auto *save = gtk_button_new_with_label("Save Label");
    gtk_box_append(GTK_BOX(buttons), cancel); gtk_box_append(GTK_BOX(buttons), save);
    gtk_box_append(GTK_BOX(box), buttons);
    g_signal_connect_object(cancel, "clicked", G_CALLBACK(gtk_window_destroy), state->dialog, G_CONNECT_SWAPPED);
    g_signal_connect_object(save, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
        auto *dialog = static_cast<BookmarkLabelDialog *>(
            g_object_get_data(G_OBJECT(data), "ifm-bookmark-label"));
        auto *window = GTK_WINDOW(g_weak_ref_get(&dialog->owner));
        auto *self = window != nullptr ? static_cast<FileManagerWindow *>(
            g_object_get_data(G_OBJECT(window), "ifm-window-controller")) : nullptr;
        if (self == nullptr || gtk_window_get_application(window) == nullptr) {
            g_clear_object(&window);
            return;
        }
        const char *label = gtk_editable_get_text(GTK_EDITABLE(dialog->entry));
        if (self->rename_bookmark(dialog->uri, label != nullptr ? label : "")) {
            gtk_window_destroy(GTK_WINDOW(dialog->dialog));
        } else {
            gtk_label_set_text(GTK_LABEL(dialog->message), "The label could not be saved. Check that the bookmark still exists and the configuration folder is writable.");
        }
        g_clear_object(&window);
    }), state->dialog, G_CONNECT_DEFAULT);
    gtk_window_set_default_widget(GTK_WINDOW(state->dialog), save);
    gtk_entry_set_activates_default(GTK_ENTRY(state->entry), TRUE);
    gtk_window_present(GTK_WINDOW(state->dialog));
    gtk_widget_grab_focus(state->entry);
}

GType FileManagerWindow::bookmark_drag_type()
{
    static const GType type = g_boxed_type_register_static("InfiltratorFilesBookmarkDrag",
        +[](gpointer data) -> gpointer { return new BookmarkDrag(*static_cast<BookmarkDrag *>(data)); },
        +[](gpointer data) { delete static_cast<BookmarkDrag *>(data); });
    return type;
}

void FileManagerWindow::install_bookmark_drag(GtkWidget *row)
{
    auto *drag = gtk_drag_source_new();
    gtk_drag_source_set_actions(drag, GDK_ACTION_MOVE);
    g_signal_connect(drag, "prepare", G_CALLBACK(+[](GtkDragSource *source, double, double,
                                                    gpointer data) -> GdkContentProvider * {
        auto *self = static_cast<FileManagerWindow *>(data);
        if (gtk_window_get_application(self->native_window()) == nullptr) return nullptr;
        auto *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
        BookmarkDrag payload(self->native_window(), widget);
        return gdk_content_provider_new_typed(bookmark_drag_type(), &payload);
    }), this);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(drag));
    auto *drop = gtk_drop_target_new(bookmark_drag_type(), GDK_ACTION_MOVE);
    g_signal_connect(drop, "drop", G_CALLBACK(+[](GtkDropTarget *target, const GValue *value,
                                                 double, double y, gpointer data) -> gboolean {
        auto *self = static_cast<FileManagerWindow *>(data);
        if (!self->bookmarks_writable_ || !G_VALUE_HOLDS(value, bookmark_drag_type())) return FALSE;
        auto *payload = static_cast<BookmarkDrag *>(g_value_get_boxed(value));
        if (payload == nullptr) return FALSE;
        auto *window = GTK_WINDOW(g_weak_ref_get(&payload->owner));
        auto *source_row = GTK_WIDGET(g_weak_ref_get(&payload->row));
        const char *uri = source_row != nullptr ? static_cast<const char *>(
            g_object_get_data(G_OBJECT(source_row), "ifm-target")) : nullptr;
        auto *app = gtk_window_get_application(self->native_window());
        bool moved = false;
        if (uri != nullptr && window != nullptr && app != nullptr &&
            gtk_window_get_application(window) == app) {
            auto *target_row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
            auto &items = self->bookmarks_;
            const auto from = std::find_if(items.begin(), items.end(), [&](const BookmarkRow &item) { return item.uri == uri; });
            const auto to = std::find_if(items.begin(), items.end(), [&](const BookmarkRow &item) { return item.row == target_row; });
            if (from != items.end() && to != items.end()) {
                auto index = static_cast<std::size_t>(to - items.begin());
                if (y > gtk_widget_get_height(target_row) / 2.0) ++index;
                if (from < items.begin() + static_cast<std::ptrdiff_t>(index)) --index;
                BookmarkRow item = std::move(*from);
                items.erase(from);
                items.insert(items.begin() + static_cast<std::ptrdiff_t>(index), std::move(item));
                self->reorder_bookmarks(); moved = self->save_bookmarks();
            }
        }
        g_clear_object(&source_row); g_clear_object(&window);
        return moved;
    }), this);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(drop));
}
} // namespace infiltrator::files
