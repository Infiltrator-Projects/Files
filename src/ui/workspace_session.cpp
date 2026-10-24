// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_manager_window.hpp"
#include "presentation_store.hpp"

#include <algorithm>
#include <cstring>
#include <memory>

namespace infiltrator::files {
namespace {
using Keys = std::unique_ptr<GKeyFile, decltype(&g_key_file_unref)>;
bool valid_uri(const char *value)
{
    if (value == nullptr || *value == '\0' || std::strlen(value) > 16384U) return false;
    char *scheme = g_uri_parse_scheme(value);
    const bool valid = scheme != nullptr;
    g_free(scheme); return valid;
}
bool integer(GKeyFile *keys, const char *group, const char *name, int min, int max, int &value)
{
    GError *error = nullptr;
    value = g_key_file_get_integer(keys, group, name, &error);
    const bool valid = error == nullptr && value >= min && value <= max;
    g_clear_error(&error); return valid;
}
}

void FileManagerWindow::schedule_session_checkpoint()
{
    if (!session_checkpoint_enabled_ || native_window() == nullptr ||
        gtk_window_get_application(native_window()) == nullptr)
        return;
    if (session_checkpoint_timeout_ != 0U) {
        g_source_remove(session_checkpoint_timeout_);
        session_checkpoint_timeout_ = 0U;
    }
    // Bounded, browsing-only snapshot: the existing strict v1 serializer
    // never captures pending file operations or promotes journal entries.
    auto *weak = new GWeakRef;
    g_weak_ref_init(weak, G_OBJECT(native_window()));
    session_checkpoint_timeout_ = g_timeout_add_seconds_full(
        G_PRIORITY_DEFAULT_IDLE, 2, +[](gpointer ptr) -> gboolean {
            auto *ref = static_cast<GWeakRef *>(ptr);
            GtkWindow *window = GTK_WINDOW(g_weak_ref_get(ref));
            auto *self = window != nullptr ? static_cast<FileManagerWindow *>(
                g_object_get_data(G_OBJECT(window), "ifm-window-controller")) : nullptr;
            if (self != nullptr && gtk_window_get_application(window) != nullptr) {
                self->session_checkpoint_timeout_ = 0U;
                (void)self->save_session();
            }
            g_clear_object(&window);
            return G_SOURCE_REMOVE;
        }, weak, +[](gpointer ptr) {
            auto *ref = static_cast<GWeakRef *>(ptr);
            g_weak_ref_clear(ref);
            delete ref;
        });
}

bool FileManagerWindow::save_session()
{
    if (tabs_.empty() || tabs_.size() > 128U || active_tab_ >= tabs_.size()) return false;
    remember_tab();
    Keys keys(g_key_file_new(), g_key_file_unref);
    g_key_file_set_integer(keys.get(), "Session", "version", 1);
    g_key_file_set_integer(keys.get(), "Session", "tabs", static_cast<int>(tabs_.size()));
    g_key_file_set_integer(keys.get(), "Session", "active", static_cast<int>(active_tab_));
    int width = 0, height = 0;
    gtk_window_get_default_size(native_window(), &width, &height);
    g_key_file_set_integer(keys.get(), "Session", "width", std::clamp(width, 640, 8192));
    g_key_file_set_integer(keys.get(), "Session", "height", std::clamp(height, 400, 8192));
    g_key_file_set_boolean(keys.get(), "Session", "maximized", gtk_window_is_maximized(native_window()));
    for (std::size_t i = 0; i < tabs_.size(); ++i) {
        const auto &tab = tabs_[i];
        const std::string group = "Tab " + std::to_string(i);
        g_key_file_set_string(keys.get(), group.c_str(), "uri", tab.uri.c_str());
        g_key_file_set_integer(keys.get(), group.c_str(), "view", static_cast<int>(tab.view));
        // Keep a bounded useful Back/Forward neighbourhood, retaining the
        // current index rather than treating the latest visit as current.
        const auto start = tab.index > 63U ? tab.index - 63U : 0U;
        const auto end = std::min(tab.history.size(), start + 128U);
        std::vector<const char *> history;
        for (auto n = start; n < end; ++n) history.push_back(tab.history[n].uri.c_str());
        g_key_file_set_string_list(keys.get(), group.c_str(), "history", history.data(), history.size());
        g_key_file_set_integer(keys.get(), group.c_str(), "index", static_cast<int>(tab.index - start));
        std::vector<std::string> encoded;
        std::vector<const char *> names;
        for (const auto &name : tab.selected_names) {
            if (encoded.size() == 1024U) break;
            char *text = g_base64_encode(reinterpret_cast<const guchar *>(name.data()), name.size());
            encoded.emplace_back(text); g_free(text);
        }
        for (const auto &name : encoded) names.push_back(name.c_str());
        g_key_file_set_string_list(keys.get(), group.c_str(), "selection", names.data(), names.size());
    }
    gsize bytes = 0;
    char *contents = g_key_file_to_data(keys.get(), &bytes, nullptr);
    std::string error;
    const bool saved = contents != nullptr && presentation_store::write("session.ini", {contents, bytes}, error);
    g_free(contents);
    if (!saved) g_warning("Files could not save its browsing session: %s", error.c_str());
    return saved;
}

bool FileManagerWindow::restore_previous_session()
{
    std::string contents, error;
    const auto status = presentation_store::read("session.ini", contents, error);
    if (status == presentation_store::ReadStatus::Missing) return false;
    Keys keys(g_key_file_new(), g_key_file_unref);
    int version = 0, count = 0, active = 0, width = 0, height = 0;
    bool valid = status == presentation_store::ReadStatus::Loaded &&
        g_key_file_load_from_data(keys.get(), contents.data(), contents.size(), G_KEY_FILE_NONE, nullptr) &&
        integer(keys.get(), "Session", "version", 1, 1, version) &&
        integer(keys.get(), "Session", "tabs", 1, 128, count) &&
        integer(keys.get(), "Session", "active", 0, count - 1, active) &&
        integer(keys.get(), "Session", "width", 640, 8192, width) &&
        integer(keys.get(), "Session", "height", 400, 8192, height);
    std::vector<TabState> restored;
    for (int i = 0; valid && i < count; ++i) {
        const std::string group = "Tab " + std::to_string(i);
        char *uri = g_key_file_get_string(keys.get(), group.c_str(), "uri", nullptr);
        int view = 0, index = 0;
        gsize history_count = 0, selection_count = 0;
        char **history = g_key_file_get_string_list(keys.get(), group.c_str(), "history", &history_count, nullptr);
        char **names = g_key_file_get_string_list(keys.get(), group.c_str(), "selection", &selection_count, nullptr);
        valid = valid_uri(uri) && integer(keys.get(), group.c_str(), "view", 0, 2, view) &&
            history != nullptr && history_count > 0U && history_count <= 128U &&
            integer(keys.get(), group.c_str(), "index", 0, static_cast<int>(history_count) - 1, index) &&
            names != nullptr && selection_count <= 1024U;
        TabState tab;
        if (valid) {
            tab.uri = uri; tab.index = static_cast<std::size_t>(index); tab.view = static_cast<ViewMode>(view);
            for (gsize n = 0; valid && n < history_count; ++n) {
                valid = valid_uri(history[n]);
                if (!valid) break;
                GFile *file = g_file_new_for_uri(history[n]);
                char *label = g_file_get_parse_name(file);
                tab.history.push_back(Location{history[n], label != nullptr ? label : history[n],
                                                classify_location_uri(history[n])});
                g_free(label); g_object_unref(file);
            }
            for (gsize n = 0; valid && n < selection_count; ++n) {
                gsize bytes = 0;
                guchar *decoded = g_base64_decode(names[n], &bytes);
                const std::string name(reinterpret_cast<const char *>(decoded), bytes);
                char *canonical = g_base64_encode(decoded, bytes);
                valid = bytes > 0U && bytes <= 4096U && name != "." && name != ".." &&
                    name.find('/') == std::string::npos && name.find('\0') == std::string::npos &&
                    g_strcmp0(canonical, names[n]) == 0;
                g_free(canonical); g_free(decoded);
                if (valid) tab.selected_names.push_back(name);
            }
            valid = valid && tab.history[tab.index].uri == tab.uri;
        }
        g_free(uri); g_strfreev(history); g_strfreev(names);
        if (valid) restored.push_back(std::move(tab));
    }
    if (!valid || restored.empty()) {
        gtk_label_set_text(GTK_LABEL(status_label_),
            "The saved browsing session could not be restored. The current folder remains available.");
        return false;
    }
    // No session data can dispatch an operation. Inactive folders are not
    // probed; only the active tab is enumerated asynchronously by GTK/GIO.
    tabs_ = std::move(restored);
    active_tab_ = static_cast<std::size_t>(active);
    recently_closed_tabs_.clear();
    gtk_window_set_default_size(native_window(), width, height);
    if (g_key_file_get_boolean(keys.get(), "Session", "maximized", nullptr))
        gtk_window_maximize(native_window());
    display_active_tab();
    return true;
}
} // namespace infiltrator::files
