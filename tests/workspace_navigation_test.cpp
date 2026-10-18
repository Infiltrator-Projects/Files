// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/ui/file_manager_window.hpp"

#include <csignal>
#include <execinfo.h>
#include <unistd.h>
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
    static void search(FileManagerWindow &w, const char *query) {
        w.show_search();
        gtk_editable_set_text(GTK_EDITABLE(w.search_entry_), query);
    }
    static std::size_t recents(FileManagerWindow &w) { return w.recent_locations_.size(); }
    static void clear_recents(FileManagerWindow &w) { FileManagerWindow::on_recent_clear(nullptr, &w); }
    static guint visible_count(FileManagerWindow &w) {
        GListModel *model = gtk_multi_selection_get_model(w.selection_);
        return model != nullptr ? g_list_model_get_n_items(model) : 0U;
    }
    static void close(FileManagerWindow &w) { w.close_tab(w.active_tab_); }
    static void duplicate(FileManagerWindow &w) { w.duplicate_active_tab(); }
    static void reopen(FileManagerWindow &w) { w.reopen_closed_tab(); }
    static void move_left(FileManagerWindow &w) { w.move_active_tab(-1); }
    static std::size_t active(FileManagerWindow &w) { return w.active_tab_; }
    static std::size_t history(const FileManagerWindow &w) { return w.history_.size(); }
    static std::size_t bookmarks(const FileManagerWindow &w) { return w.bookmarks_.size(); }
    static void pin(FileManagerWindow &w) { FileManagerWindow::on_bookmark_toggle(nullptr, &w); }
    static bool rename(FileManagerWindow &w) { return w.begin_inline_rename(); }
    static GtkWidget *find_editor(GtkWidget *widget)
    {
        if (widget == nullptr) return nullptr;
        if (GTK_IS_ENTRY(widget) && GTK_IS_STACK(gtk_widget_get_parent(widget)) &&
            gtk_stack_get_visible_child(GTK_STACK(gtk_widget_get_parent(widget))) == widget)
            return widget;
        for (GtkWidget *child = gtk_widget_get_first_child(widget);
             child != nullptr; child = gtk_widget_get_next_sibling(child)) {
            if (GtkWidget *editor = find_editor(child); editor != nullptr) return editor;
        }
        return nullptr;
    }
    static GtkWidget *editor(FileManagerWindow &w) { return find_editor(w.content_stack_); }
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
bool directory_contains_name(FileManagerWindow &w, const char *name)
{
    // Filesystem existence alone does not prove GTK's asynchronous monitor
    // has delivered its new entry. Verify the visible model as well.
    GListModel *model = G_LIST_MODEL(FileManagerWindowTestAccess::directory(w));
    if (model == nullptr) return false;
    for (guint i = 0; i < g_list_model_get_n_items(model); ++i) {
        auto *info = G_FILE_INFO(g_list_model_get_item(model, i));
        const bool found = info != nullptr &&
                           g_strcmp0(g_file_info_get_name(info), name) == 0;
        if (info != nullptr) g_object_unref(info);
        if (found) return true;
    }
    return false;
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
GtkWidget *find_button(GtkWidget *root, const char *label)
{
    if (root == nullptr) return nullptr;
    if (GTK_IS_BUTTON(root) &&
        g_strcmp0(gtk_button_get_label(GTK_BUTTON(root)), label) == 0) return root;
    for (GtkWidget *child = gtk_widget_get_first_child(root);
         child != nullptr; child = gtk_widget_get_next_sibling(child)) {
        if (GtkWidget *match = find_button(child, label); match != nullptr) return match;
    }
    return nullptr;
}
GtkWidget *find_menu(GtkWidget *root)
{
    if (root == nullptr) return nullptr;
    if (GTK_IS_MENU_BUTTON(root) &&
        g_strcmp0(gtk_widget_get_tooltip_text(root), "File actions") == 0)
        return root;
    for (GtkWidget *child = gtk_widget_get_first_child(root);
         child != nullptr; child = gtk_widget_get_next_sibling(child)) {
        if (GtkWidget *found = find_menu(child); found != nullptr) return found;
    }
    return nullptr;
}
GtkWidget *find_clipboard_button(FileManagerWindow &window, const char *label)
{
    GtkWidget *menu = find_menu(gtk_window_get_titlebar(window.native_window()));
    if (menu == nullptr) return nullptr;
    GtkWidget *popover = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(menu)));
    return find_button(popover, label);
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

void trace_fatal(int)
{
    void *frames[40]{};
    const int count = backtrace(frames, 40);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    _exit(99);
}
int main()
{
    std::signal(SIGTRAP, trace_fatal);
    char *temporary = g_dir_make_tmp("files-workspace-XXXXXX", nullptr);
    require(temporary != nullptr, "Failed to create test profile");
    std::filesystem::path root(temporary);
    g_free(temporary);
    const auto config = root / "config";
    g_setenv("XDG_CONFIG_HOME", config.c_str(), TRUE);
    g_setenv("XDG_STATE_HOME", (root / "state").c_str(), TRUE);
    g_setenv("HOME", root.c_str(), TRUE);
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
        std::ofstream(a / ".hidden.txt") << "Hidden";
        auto *window = new FileManagerWindow(application);
        window->install_operation_surface();
        window->present();
        FileManagerWindowTestAccess::navigate(*window, a);
        await([&] { return !gtk_directory_list_is_loading(
            FileManagerWindowTestAccess::directory(*window)) &&
            g_list_model_get_n_items(G_LIST_MODEL(
                FileManagerWindowTestAccess::directory(*window))) == 2; });
        require(FileManagerWindowTestAccess::visible_count(*window) == 1U,
                "Hidden entries must not be shown without the preference");
        window->toggle_show_hidden();
        require(FileManagerWindowTestAccess::visible_count(*window) == 2U,
                "Showing hidden files must update the presentation model");
        window->toggle_show_hidden();
        require(FileManagerWindowTestAccess::visible_count(*window) == 1U,
                "Hiding hidden files must restore the visible model");
        FileManagerWindowTestAccess::search(*window, "SELECTED");
        await([&] { return FileManagerWindowTestAccess::visible_count(*window) == 1U; });
        FileManagerWindowTestAccess::search(*window, "no-such-match");
        await([&] { return FileManagerWindowTestAccess::visible_count(*window) == 0U; });
        FileManagerWindowTestAccess::search(*window, "selected");
        await([&] { return FileManagerWindowTestAccess::visible_count(*window) == 1U; });
        require(FileManagerWindowTestAccess::recents(*window) >= 1U,
                "Visited native home folders should appear in Recent");
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
                FileManagerWindowTestAccess::directory(*window))) == 2; });
        require(FileManagerWindowTestAccess::uri(*window) == uri(a),
                "Tab activation must restore its own directory");
        require(selected_name(*window, "selected.txt"),
                "Tab activation must restore selection by source filename");
        require(FileManagerWindowTestAccess::icons_active(*window),
                "Switching back must restore the active view-mode button");
        require(FileManagerWindowTestAccess::rename(*window),
                "Selected file must have an in-place rename editor");
        GtkWidget *editor = FileManagerWindowTestAccess::editor(*window);
        require(GTK_IS_ENTRY(editor), "In-place rename must expose an active GTK entry");
        gtk_editable_set_text(GTK_EDITABLE(editor), "renamed.txt");
        g_signal_emit_by_name(editor, "activate");
        await([&] { return std::filesystem::exists(a / "renamed.txt"); });
        FileManagerWindowTestAccess::activate(*window, 1);
        require(FileManagerWindowTestAccess::uri(*window) == uri(c),
                "Second tab must preserve independent navigation");
        FileManagerWindowTestAccess::duplicate(*window);
        require(FileManagerWindowTestAccess::tabs(*window) == 3U,
                "Duplicating a tab must leave the original tabs intact");
        FileManagerWindowTestAccess::move_left(*window);
        require(FileManagerWindowTestAccess::active(*window) == 1U &&
                FileManagerWindowTestAccess::uri(*window) == uri(c),
                "Tab reorder must preserve the active directory");
        FileManagerWindowTestAccess::close(*window);
        require(FileManagerWindowTestAccess::tabs(*window) == 2U,
                "Closing a tab must remove just that tab");
        FileManagerWindowTestAccess::reopen(*window);
        require(FileManagerWindowTestAccess::tabs(*window) == 3U &&
                FileManagerWindowTestAccess::uri(*window) == uri(c),
                "Reopening a closed tab must recover its directory");
        window->toggle_show_hidden();
        window->toggle_sidebar();
        FileManagerWindowTestAccess::navigate(*window, a);
        await([&] { return !gtk_directory_list_is_loading(
            FileManagerWindowTestAccess::directory(*window)); });
        select_name(*window, "renamed.txt");
        GtkWidget *copy = find_clipboard_button(*window, "Copy");
        GtkWidget *paste = find_clipboard_button(*window, "Paste");
        require(copy != nullptr && paste != nullptr,
                "Clipboard commands must be exposed through File actions");
        std::cerr << "WORKSPACE-CLIP-STAGE 1\n";
        g_signal_emit_by_name(copy, "clicked");
        GdkClipboard *clipboard = gdk_display_get_clipboard(
            gtk_widget_get_display(GTK_WIDGET(window->native_window())));
        require(clipboard != nullptr, "Test requires a display clipboard");
        GdkContentFormats *formats = gdk_clipboard_get_formats(clipboard);
        require(formats != nullptr &&
                gdk_content_formats_contain_mime_type(formats, "x-special/gnome-copied-files") &&
                gdk_content_formats_contain_mime_type(formats, "text/uri-list"),
                "File Copy must publish GNOME cut/copy and standard URI-list formats");
        std::cerr << "WORKSPACE-CLIP-STAGE 2\n";
        gdk_clipboard_set_text(clipboard, "not a file reference");
        FileManagerWindowTestAccess::navigate(*window, b);
        std::cerr << "WORKSPACE-CLIP-STAGE 5\n";
        g_signal_emit_by_name(paste, "clicked");
        await([&] { return !gtk_directory_list_is_loading(
            FileManagerWindowTestAccess::directory(*window)); });
        while (g_main_context_iteration(nullptr, FALSE)) {}
        std::cerr << "WORKSPACE-CLIP-STAGE 3\n";
        require(!std::filesystem::exists(b / "renamed.txt"),
                "Paste must not act on an old file after clipboard ownership changes");
        // Simulate a separate GNOME-compatible file manager offering file refs.
        std::cerr << "WORKSPACE-CLIP-STAGE 4\n";
        const std::string external_copy = "copy\n" + uri(a / "renamed.txt") + "\n";
        GBytes *bytes = g_bytes_new(external_copy.data(), external_copy.size());
        GdkContentProvider *external = gdk_content_provider_new_for_bytes(
            "x-special/gnome-copied-files", bytes);
        require(gdk_clipboard_set_content(clipboard, external),
                "Clipboard test must own a GNOME file-reference offer");
        g_object_unref(external);
        g_bytes_unref(bytes);
        g_signal_emit_by_name(paste, "clicked");
        std::cerr << "WORKSPACE-CLIP-STAGE 6\n";
        await([&] { return std::filesystem::is_regular_file(b / "renamed.txt"); });
        require(std::filesystem::is_regular_file(a / "renamed.txt"),
                "External GNOME Copy must preserve the source file");
        // Exercise the actual model refresh, rather than racing the pending
        // GFileMonitor query by closing immediately after the disk write.
        await([&] { return directory_contains_name(*window, "renamed.txt"); });
        std::cerr << "WORKSPACE-CLIP-STAGE 7\n";
        gtk_window_destroy(window->native_window());
        while (g_main_context_iteration(nullptr, FALSE)) {}
        std::cerr << "WORKSPACE-CLIP-STAGE 8\n";
        auto *reopened = new FileManagerWindow(application);
        require(FileManagerWindowTestAccess::bookmarks(*reopened) == 1,
                "Bookmark must survive a new window and profile read");
        require(FileManagerWindowTestAccess::recents(*reopened) >= 1U,
                "Recent locations must survive new windows and process reinitialisation");
        FileManagerWindowTestAccess::clear_recents(*reopened);
        require(FileManagerWindowTestAccess::recents(*reopened) == 0U,
                "Clearing recent history must empty the in-memory list");
        require(reopened->show_hidden(),
                "Hidden-files preference must survive application restart");
        require(!reopened->sidebar_visible(),
                "Sidebar preference must survive application restart");
        require(FileManagerWindowTestAccess::icons_active(*reopened),
                "Default view preference must survive application restart");
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
