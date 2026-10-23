// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_manager_window.hpp"

#include <memory>

namespace infiltrator::files {
namespace {
struct LocationRequest {
    GWeakRef window;
    guint64 generation;
    LocationRequest(GtkWindow *owner, guint64 value) : generation(value)
    {
        g_weak_ref_init(&window, G_OBJECT(owner));
    }
    ~LocationRequest() { g_weak_ref_clear(&window); }
};
}

void FileManagerWindow::open_location(GFile *file)
{
    if (file == nullptr || gtk_window_get_application(native_window()) == nullptr) return;
    if (location_request_ != nullptr) g_cancellable_cancel(location_request_);
    g_clear_object(&location_request_);
    location_request_ = g_cancellable_new();
    ++navigation_generation_;
    pending_tab_selection_.clear();
    gtk_label_set_text(GTK_LABEL(status_label_), "Opening location…");
    auto *request = new LocationRequest(native_window(), navigation_generation_);
    g_file_query_info_async(file, G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_NAME,
                           G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, location_request_,
                           on_location_inspected, request);
}

void FileManagerWindow::on_location_inspected(GObject *source, GAsyncResult *result, gpointer data)
{
    std::unique_ptr<LocationRequest> request(static_cast<LocationRequest *>(data));
    GError *error = nullptr;
    GFileInfo *info = g_file_query_info_finish(G_FILE(source), result, &error);
    auto *owner = GTK_WINDOW(g_weak_ref_get(&request->window));
    auto *self = owner != nullptr ? static_cast<FileManagerWindow *>(
        g_object_get_data(G_OBJECT(owner), "ifm-window-controller")) : nullptr;
    if (self != nullptr && gtk_window_get_application(owner) != nullptr &&
        request->generation == self->navigation_generation_) {
        g_clear_object(&self->location_request_);
        if (info == nullptr) {
            if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                const std::string message = std::string("The location could not be opened: ") +
                    (error != nullptr ? error->message : "metadata is unavailable");
                gtk_label_set_text(GTK_LABEL(self->status_label_), message.c_str());
            }
        } else if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
            self->navigate_to(G_FILE(source), true);
        } else {
            GFile *parent = g_file_get_parent(G_FILE(source));
            const char *name = g_file_info_get_name(info);
            if (parent != nullptr && name != nullptr) {
                self->navigate_to(parent, true);
                self->pending_tab_selection_ = {name};
                self->restore_tab_selection();
            } else {
                gtk_label_set_text(GTK_LABEL(self->status_label_),
                                   "This item has no browsable parent folder.");
            }
            g_clear_object(&parent);
        }
    }
    g_clear_object(&info);
    g_clear_error(&error);
    g_clear_object(&owner);
}
} // namespace infiltrator::files
