// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/ui/file_manager_window.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace infiltrator::files {
struct FileManagerWindowTestAccess {
    static void navigate(FileManagerWindow &w, const std::filesystem::path &path)
    {
        GFile *file = g_file_new_for_path(path.c_str());
        w.navigate_to(file, true);
        g_object_unref(file);
    }
    static void add(FileManagerWindow &w, const std::filesystem::path &path)
    {
        GFile *file = g_file_new_for_path(path.c_str());
        w.add_tab(file);
        g_object_unref(file);
    }
    static void activate(FileManagerWindow &w, std::size_t i) { w.activate_tab(i); }
    static std::size_t tabs(const FileManagerWindow &w) { return w.tabs_.size(); }
    static std::size_t history(const FileManagerWindow &w) { return w.history_.size(); }
    static std::size_t bookmarks(const FileManagerWindow &w) { return w.bookmarks_.size(); }
    static void pin(FileManagerWindow &w) { FileManagerWindow::on_bookmark_toggle(nullptr, &w); }
    static bool rename(FileManagerWindow &w) { return w.begin_inline_rename(); }
    static void set_icons(FileManagerWindow &w) { w.set_view_mode(FileManagerWindow::ViewMode::Icons); }
    static bool icons_active(FileManagerWindow &w) {
        return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w.icon_view_button_));
    }
    static GtkDirectoryList *directory(FileManagerWindow &w) { return w.directory_list_; }
    static GtkMultiSelection *selection(FileManagerWindow &w) { return w.selection_; }
    static std::string uri(FileManagerWindow &w)
    {
        char *value = g_file_get_uri(w.current_location_);
        std::string result = value != nullptr ? value : "";
        g_free(value);
        return result;
    }
};
} // namespace infiltrator::files

using namespace infiltrator::files;

namespace {
void require(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}
void await(const std::function<bool()> &ready)
{
    const gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!ready() && g_get_monotonic_time() < deadline) {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        g_usleep(1000);
    }
    require(ready(), "Timed out waiting for directory model");
}
std::string uri(const std::filesystem::path &path)
{
    GFile *file = g_file_new_for_path(path.c_str());
    char *text = g_file_get_uri(file);
    std::string result(text);
    g_free(text);
    g_object_unref(file);
    return result;
}
void select_name(FileManagerWindow &w, const char *name)
{
    GtkMultiSelection *selection = FileManagerWindowTestAccess::selection(w);
    GListModel *model = gtk_multi_selection_get_model(selection);
    for (guint i = 0; i < g_list_model_get_n_items(model); ++i) {
        auto *info = G_FILE_INFO(g_list_model_get_item(model, i));
        const bool found = info != nullptr &&
                           g_strcmp0(g_file_info_get_name(info), name) == 0;
        if (info != nullptr) g_object_unref(info);
        if (found) {
            gtk_selection_model_select_item(GTK_SELECTION_MODEL(selection), i, TRUE);
            return;
        }
    }
    throw std::runtime_error("Test file not visible");
}
bool selected_name(FileManagerWindow &w, const char *name)
{
    GtkMultiSelection *selection = FileManagerWindowTestAccess::selection(w);
    GListModel *model = gtk_multi_selection_get_model(selection);
    GtkBitset *bits = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(selection));
    bool match = false;
    for (guint i = 0; i < g_list_model_get_n_items(model); ++i) {
        if (!gtk_bitset_contains(bits, i)) continue;
        auto *info = G_FILE_INFO(g_list_model_get_item(model, i));
        match = info != nullptr && g_strcmp0(g_file_info_get_name(info), name) == 0;
        if (info != nullptr) g_object_unref(info);
        if (match) break;
    }
    gtk_bitset_unref(bits);
    return match;
}
} // namespace

int main()
{
    char *temporary = g_dir_make_tmp("files-workspace-XXXXXX", nullptr);
    require(temporary != nullptr, "Failed to create test profile");
    std::filesystem::path root(temporary);
    g_free(temporary);
    const auto config = root / "config";
    g_setenv("XDG_CONFIG_HOME", config.c_str(), TRUE);
    g_setenv("GTK_A11Y", "none", TRUE);
    g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
    gtk_init();
    GtkApplication *application = gtk_application_new(
        "org.infiltrator.Files.WorkspaceTest", G_APPLICATION_NON_UNIQUE);
    require(g_application_register(G_APPLICATION(application), nullptr, nullptr),
            "Failed to register application");
    try {
        const auto a = root / "a";
        const auto b = root / "b";
        const auto c = root / "c";
        std::filesystem::create_directories(a);
        std::filesystem::create_directories(b);
        std::filesystem::create_directories(c);
        std::ofstream(a / "selected.txt") << "Selected";
        auto *window = new FileManagerWindow(application);
        window->install_operation_surface();
        window->present();
        FileManagerWindowTestAccess::navigate(*window, a);
        await([&] { return !gtk_directory_list_is_loading(
            FileManagerWindowTestAccess::directory(*window)) &&
            g_list_model_get_n_items(G_LIST_MODEL(
                FileManagerWindowTestAccess::directory(*window))) == 1; });
        select_name(*window, "selected.txt");
        FileManagerWindowTestAccess::pin(*window);
        FileManagerWindowTestAccess::set_icons(*window);
        require(FileManagerWindowTestAccess::bookmarks(*window) == 1,
                "Pinning must append one bookmark");
        FileManagerWindowTestAccess::add(*window, b);
        require(FileManagerWindowTestAccess::tabs(*window) == 2,
                "New tab must preserve existing tab");
        FileManagerWindowTestAccess::navigate(*window, c);
        require(FileManagerWindowTestAccess::history(*window) == 2,
                "New tab must retain its own local history");
        require(FileManagerWindowTestAccess::icons_active(*window),
                "New tab must inherit the current presentation mode");
        FileManagerWindowTestAccess::activate(*window, 0);
        await([&] { return !gtk_directory_list_is_loading(
            FileManagerWindowTestAccess::directory(*window)) &&
            g_list_model_get_n_items(G_LIST_MODEL(
                FileManagerWindowTestAccess::directory(*window))) == 1; });
        require(FileManagerWindowTestAccess::uri(*window) == uri(a),
                "Tab activation must restore its own directory");
        require(selected_name(*window, "selected.txt"),
                "Tab activation must restore selection by source filename");
        require(FileManagerWindowTestAccess::icons_active(*window),
                "Switching back must restore the active view-mode button");
        require(FileManagerWindowTestAccess::rename(*window),
                "Selected file must have an in-place rename editor");
        GtkWidget *editor = gtk_window_get_focus(window->native_window());
        require(GTK_IS_ENTRY(editor), "In-place rename must focus a GTK entry");
        gtk_editable_set_text(GTK_EDITABLE(editor), "renamed.txt");
        g_signal_emit_by_name(editor, "activate");
        await([&] { return std::filesystem::exists(a / "renamed.txt"); });
        FileManagerWindowTestAccess::activate(*window, 1);
        require(FileManagerWindowTestAccess::uri(*window) == uri(c),
                "Second tab must preserve independent navigation");
        gtk_window_destroy(window->native_window());
        while (g_main_context_iteration(nullptr, FALSE)) {}
        auto *reopened = new FileManagerWindow(application);
        require(FileManagerWindowTestAccess::bookmarks(*reopened) == 1,
                "Bookmark must survive a new window and profile read");
        gtk_window_destroy(reopened->native_window());
        while (g_main_context_iteration(nullptr, FALSE)) {}
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        g_object_unref(application);
        return 1;
    }
    g_object_unref(application);
    std::filesystem::remove_all(root);
    return 0;
}
