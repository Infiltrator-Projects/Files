// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_manager_window.hpp"
#include "presentation_store.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace infiltrator::files {
namespace {
constexpr std::size_t kMaximumFolderViews = 128U;
bool valid_uri(const std::string &uri)
{
    if (uri.empty() || uri.size() > 16384U || uri.find('\0') != std::string::npos)
        return false;
    char *scheme = g_uri_parse_scheme(uri.c_str());
    const bool valid = scheme != nullptr;
    g_free(scheme);
    return valid;
}
} // namespace

void FileManagerWindow::load_folder_views()
{
    std::string contents, error;
    const auto status = presentation_store::read("folder-views.ini", contents, error);
    if (status == presentation_store::ReadStatus::Missing) {
        folder_views_.clear();
        return;
    }
    if (status == presentation_store::ReadStatus::Failed) {
        folder_views_writable_ = false;
        return;
    }
    GKeyFile *keys = g_key_file_new();
    GError *parse_error = nullptr;
    bool valid = g_key_file_load_from_data(keys, contents.data(), contents.size(),
                                           G_KEY_FILE_NONE, &parse_error);
    int count = -1;
    if (valid) count = g_key_file_get_integer(keys, "Folder Views", "count", &parse_error);
    valid = valid && parse_error == nullptr && count >= 0 &&
        count <= static_cast<int>(kMaximumFolderViews) &&
        g_key_file_get_integer(keys, "Folder Views", "version", nullptr) == 1;
    std::map<std::string, ViewMode> parsed;
    for (int i = 0; valid && i < count; ++i) {
        const std::string group = "Folder " + std::to_string(i);
        char *uri = g_key_file_get_string(keys, group.c_str(), "uri", nullptr);
        const int mode = g_key_file_get_integer(keys, group.c_str(), "view", nullptr);
        valid = uri != nullptr && valid_uri(uri) && mode >= 1 && mode <= 3;
        if (valid)
            valid = parsed.emplace(uri, static_cast<ViewMode>(mode - 1)).second;
        g_free(uri);
    }
    g_clear_error(&parse_error);
    g_key_file_unref(keys);
    if (!valid) {
        folder_views_writable_ = false;
        return;
    }
    folder_views_ = std::move(parsed);
    folder_views_writable_ = true;
}

bool FileManagerWindow::save_folder_views() const
{
    if (!folder_views_writable_) return false;
    GKeyFile *keys = g_key_file_new();
    g_key_file_set_integer(keys, "Folder Views", "version", 1);
    g_key_file_set_integer(keys, "Folder Views", "count",
                           static_cast<int>(folder_views_.size()));
    int i = 0;
    for (const auto &[uri, mode] : folder_views_) {
        const std::string group = "Folder " + std::to_string(i++);
        g_key_file_set_string(keys, group.c_str(), "uri", uri.c_str());
        g_key_file_set_integer(keys, group.c_str(), "view", static_cast<int>(mode) + 1);
    }
    gsize bytes = 0U;
    char *contents = g_key_file_to_data(keys, &bytes, nullptr);
    std::string error;
    const bool saved = contents != nullptr &&
        presentation_store::write("folder-views.ini", {contents, bytes}, error);
    g_free(contents);
    g_key_file_unref(keys);
    return saved;
}

void FileManagerWindow::remember_folder_view(const ViewMode mode)
{
    if (current_location_ == nullptr || !preferences_ready_) return;
    // Merge against the single durable authority immediately before writing,
    // so a second open Files window cannot silently erase another choice.
    load_folder_views();
    if (!folder_views_writable_) {
        gtk_label_set_text(GTK_LABEL(status_label_),
            "Saved folder views cannot be read; the existing file is preserved.");
        return;
    }
    char *uri = g_file_get_uri(current_location_);
    if (uri == nullptr) return;
    const std::string key(uri);
    g_free(uri);
    if (!valid_uri(key)) return;
    const auto existing = folder_views_.find(key);
    if (existing != folder_views_.end() && existing->second == mode) return;
    if (existing == folder_views_.end() && folder_views_.size() >= kMaximumFolderViews) {
        gtk_label_set_text(GTK_LABEL(status_label_),
            "The 128-folder view preference limit has been reached.");
        return;
    }
    const auto previous = folder_views_;
    folder_views_[key] = mode;
    if (!save_folder_views()) {
        folder_views_ = previous;
        gtk_label_set_text(GTK_LABEL(status_label_), "Folder view preference could not be saved.");
        return;
    }
    GtkApplication *app = gtk_window_get_application(native_window());
    for (GList *node = app != nullptr ? gtk_application_get_windows(app) : nullptr;
         node != nullptr; node = node->next) {
        auto *other = static_cast<FileManagerWindow *>(
            g_object_get_data(G_OBJECT(node->data), "ifm-window-controller"));
        if (other != nullptr && other != this) other->load_folder_views();
    }
}
} // namespace infiltrator::files
