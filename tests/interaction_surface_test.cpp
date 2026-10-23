// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/ui/file_manager_window.hpp"

#include <gtk/gtk.h>

#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <stdexcept>

namespace infiltrator::files {
struct FileManagerWindowTestAccess {
    static GtkMultiSelection *selection(FileManagerWindow &w) { return w.selection_; }
    static GtkDirectoryList *listing(FileManagerWindow &w) { return w.directory_list_; }
    static std::size_t tabs(const FileManagerWindow &w) { return w.tabs_.size(); }
};
}

namespace {

void require(bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool tooltip_equals(GtkWidget *widget, const char *expected)
{
    const char *tooltip = gtk_widget_get_tooltip_text(widget);
    return g_strcmp0(tooltip, expected) == 0;
}

GtkWidget *find_by_tooltip(GtkWidget *root, const char *tooltip)
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
        if (GtkWidget *match = find_by_tooltip(child, tooltip); match != nullptr) {
            return match;
        }
    }
    return nullptr;
}

GtkWidget *find_button(GtkWidget *root, const char *label)
{
    if (root == nullptr) {
        return nullptr;
    }
    if (GTK_IS_BUTTON(root) &&
        g_strcmp0(gtk_button_get_label(GTK_BUTTON(root)), label) == 0) {
        return root;
    }
    for (GtkWidget *child = gtk_widget_get_first_child(root);
         child != nullptr;
         child = gtk_widget_get_next_sibling(child)) {
        if (GtkWidget *match = find_button(child, label); match != nullptr) {
            return match;
        }
    }
    return nullptr;
}


guint position_for(GtkMultiSelection *selection, const char *name)
{
    GListModel *model = gtk_multi_selection_get_model(selection);
    for (guint i = 0; i < g_list_model_get_n_items(model); ++i) {
        auto *info = G_FILE_INFO(g_list_model_get_item(model, i));
        const bool found = info != nullptr &&
            g_strcmp0(g_file_info_get_name(info), name) == 0;
        if (info != nullptr) g_object_unref(info);
        if (found) return i;
    }
    throw std::runtime_error(std::string("Missing test item: ") + name);
}

guint selected_count(GtkMultiSelection *selection)
{
    GtkBitset *bits = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(selection));
    const guint count = static_cast<guint>(gtk_bitset_get_size(bits));
    gtk_bitset_unref(bits);
    return count;
}

GtkWidget *find_entry(GtkWidget *root)
{
    if (root == nullptr) return nullptr;
    if (GTK_IS_ENTRY(root)) return root;
    for (GtkWidget *child = gtk_widget_get_first_child(root); child != nullptr;
         child = gtk_widget_get_next_sibling(child)) {
        if (GtkWidget *result = find_entry(child); result != nullptr) return result;
    }
    return nullptr;
}

GtkWindow *pattern_dialog()
{
    GList *top = gtk_window_list_toplevels();
    GtkWindow *result = nullptr;
    for (GList *it = top; it != nullptr; it = it->next) {
        GtkWindow *win = GTK_WINDOW(it->data);
        if (g_strcmp0(gtk_window_get_title(win), "Select by filename pattern") == 0) {
            result = win;
            break;
        }
    }
    g_list_free(top);
    return result;
}

std::string clipboard_text(GtkWindow *window)
{
    GdkClipboard *clipboard = gdk_display_get_clipboard(
        gtk_widget_get_display(GTK_WIDGET(window)));
    GdkContentProvider *content = gdk_clipboard_get_content(clipboard);
    require(content != nullptr, "Missing local text clipboard provider");
    GValue value = G_VALUE_INIT;
    g_value_init(&value, G_TYPE_STRING);
    GError *error = nullptr;
    const bool ok = gdk_content_provider_get_value(content, &value, &error);
    const char *text = ok ? g_value_get_string(&value) : nullptr;
    const std::string result = text != nullptr ? text : "";
    g_value_unset(&value);
    if (error != nullptr) g_error_free(error);
    require(ok, "Could not read selected clipboard text");
    return result;
}

void qualify_selection_actions(infiltrator::files::FileManagerWindow &w,
                               GtkWidget *popover)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("files-selection-" + std::to_string(g_get_monotonic_time()));
    fs::create_directories(root / "alpha");
    fs::create_directories(root / "beta");
    { std::ofstream(root / "first.pdf") << "pdf one"; }
    { std::ofstream(root / "second.pdf") << "pdf two"; }
    { std::ofstream(root / "notes.txt") << "notes"; }
    GFile *location = g_file_new_for_path(root.c_str());
    w.open_location(location);
    g_object_unref(location);
    auto *selection = infiltrator::files::FileManagerWindowTestAccess::selection(w);
    auto *listing = infiltrator::files::FileManagerWindowTestAccess::listing(w);
    bool loaded = false;
    for (int n = 0; n < 5000 && !loaded; ++n) {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        GListModel *items = gtk_multi_selection_get_model(selection);
        loaded = !gtk_directory_list_is_loading(listing) &&
                 g_list_model_get_n_items(items) == 5U;
        if (!loaded) g_usleep(1000);
    }
    require(loaded, "Test location did not enumerate five files/folders");

    GtkWidget *pattern_action = find_button(popover, "Select by filename pattern…");
    GtkWidget *type_action = find_button(popover, "Select same file type");
    GtkWidget *tabs_action = find_button(popover, "Open selected folders in tabs");
    GtkWidget *paths_action = find_button(popover, "Copy selected paths as text");
    GtkWidget *uris_action = find_button(popover, "Copy selected URIs as text");
    require(pattern_action && type_action && tabs_action && paths_action && uris_action,
            "One or more 0.1.60 selection actions are missing");

    g_signal_emit_by_name(pattern_action, "clicked");
    GtkWindow *dialog = pattern_dialog();
    require(dialog != nullptr, "Filename-pattern dialog was not opened");
    GtkWidget *pattern_entry = find_entry(GTK_WIDGET(dialog));
    require(pattern_entry != nullptr, "Filename-pattern entry is missing");
    gtk_editable_set_text(GTK_EDITABLE(pattern_entry), "*.pdf");
    GtkWidget *apply = find_button(GTK_WIDGET(dialog), "Select matches");
    require(apply != nullptr, "Filename-pattern confirmation is missing");
    g_signal_emit_by_name(apply, "clicked");
    require(selected_count(selection) == 2U &&
            gtk_selection_model_is_selected(GTK_SELECTION_MODEL(selection),
                                            position_for(selection, "first.pdf")) &&
            gtk_selection_model_is_selected(GTK_SELECTION_MODEL(selection),
                                            position_for(selection, "second.pdf")),
            "Glob selection must match only the two visible PDF names");

    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(selection));
    gtk_selection_model_select_item(GTK_SELECTION_MODEL(selection),
                                    position_for(selection, "first.pdf"), TRUE);
    g_signal_emit_by_name(type_action, "clicked");
    require(selected_count(selection) == 2U,
            "Select same type must include both PDFs but exclude text and directories");

    g_signal_emit_by_name(paths_action, "clicked");
    const std::string paths = clipboard_text(w.native_window());
    require(paths.find((root / "first.pdf").string()) != std::string::npos &&
            paths.find((root / "second.pdf").string()) != std::string::npos &&
            paths.find((root / "notes.txt").string()) == std::string::npos,
            "Native path export must include exactly the selected files");

    g_signal_emit_by_name(uris_action, "clicked");
    const std::string uris = clipboard_text(w.native_window());
    require(uris.find("file://") != std::string::npos &&
            uris.find("first.pdf") != std::string::npos &&
            uris.find("second.pdf") != std::string::npos,
            "URI export must enumerate selected files");

    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(selection));
    gtk_selection_model_select_item(GTK_SELECTION_MODEL(selection),
                                    position_for(selection, "alpha"), TRUE);
    gtk_selection_model_select_item(GTK_SELECTION_MODEL(selection),
                                    position_for(selection, "beta"), FALSE);
    const std::size_t before = infiltrator::files::FileManagerWindowTestAccess::tabs(w);
    g_signal_emit_by_name(tabs_action, "clicked");
    require(infiltrator::files::FileManagerWindowTestAccess::tabs(w) == before + 2U,
            "Opening selected folders must create exactly two tabs");
    while (g_main_context_iteration(nullptr, FALSE)) {}
    fs::remove_all(root);
}

} // namespace

int main()
{
    g_setenv("GTK_A11Y", "none", TRUE);
    g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
    gtk_init();

    GtkApplication *application = gtk_application_new(
        "org.infiltrator.Files.InteractionTest", G_APPLICATION_NON_UNIQUE);
    if (!g_application_register(G_APPLICATION(application), nullptr, nullptr)) {
        std::cerr << "Cannot register test application\n";
        g_object_unref(application);
        return 1;
    }

    try {
        auto *window = new infiltrator::files::FileManagerWindow(application);
        window->install_operation_surface();
        window->present();
        while (g_main_context_iteration(nullptr, FALSE)) {}

        GtkWidget *titlebar = gtk_window_get_titlebar(window->native_window());
        require(GTK_IS_HEADER_BAR(titlebar), "Files must retain a GTK header bar");

        GtkWidget *overflow = find_by_tooltip(titlebar, "File actions");
        require(GTK_IS_MENU_BUTTON(overflow), "Consolidated File actions menu is missing");

        GtkWidget *edit_location = find_by_tooltip(titlebar, "Edit location (Ctrl+L)");
        require(GTK_IS_BUTTON(edit_location), "Breadcrumb/edit-location surface is missing");

        GtkWidget *legacy_single = find_by_tooltip(titlebar, "Selected item actions");
        GtkWidget *legacy_batch =
            find_by_tooltip(titlebar, "Actions for multiple selected items");
        GtkWidget *legacy_destructive =
            find_by_tooltip(titlebar, "Trash, restore and permanent delete");
        require(legacy_single != nullptr && !gtk_widget_get_visible(legacy_single),
                "Single-item legacy header menu must be consolidated");
        require(legacy_batch != nullptr && !gtk_widget_get_visible(legacy_batch),
                "Batch legacy header menu must be consolidated");
        require(legacy_destructive != nullptr && !gtk_widget_get_visible(legacy_destructive),
                "Destructive legacy header menu must be consolidated");

        GtkWidget *popover = GTK_WIDGET(
            gtk_menu_button_get_popover(GTK_MENU_BUTTON(overflow)));
        require(popover != nullptr, "File actions popover is missing");
        require(gtk_widget_get_sensitive(overflow),
                "File actions button must be enabled at an empty location");
        GtkWidget *menu_root = gtk_popover_get_child(GTK_POPOVER(popover));
        require(GTK_IS_BOX(menu_root), "File actions needs a menu container");
        GtkWidget *scroller = gtk_widget_get_first_child(menu_root);
        require(GTK_IS_SCROLLED_WINDOW(scroller),
                "File actions must be bounded and scrollable on small displays");
        require(gtk_scrolled_window_get_max_content_height(
                    GTK_SCROLLED_WINDOW(scroller)) > 0 &&
                gtk_scrolled_window_get_max_content_height(
                    GTK_SCROLLED_WINDOW(scroller)) <= 400,
                "File actions must fit within a laptop-height popup");
        gtk_menu_button_popup(GTK_MENU_BUTTON(overflow));
        for (int i = 0; i < 30 && !gtk_widget_get_mapped(popover); ++i) {
            while (g_main_context_iteration(nullptr, FALSE)) {}
            g_usleep(1000);
        }
        require(gtk_widget_get_mapped(popover),
                "Clicking File actions must display the native GTK popover");
        require(gtk_widget_get_height(popover) <= 500,
                "File actions popup must not exceed the compact test window height");
        require(find_button(popover, "New Folder") != nullptr,
                "New Folder must remain available after header consolidation");
        require(find_button(popover, "Properties") != nullptr,
                "Properties must be available from File actions");
        require(find_button(popover, "Copy") != nullptr,
                "Clipboard Copy must be available from File actions");
        require(find_button(popover, "Cut") != nullptr,
                "Clipboard Cut must be available from File actions");
        require(find_button(popover, "Paste") != nullptr,
                "Clipboard Paste must be available from File actions");
        gtk_menu_button_popdown(GTK_MENU_BUTTON(overflow));
        require(!gtk_widget_get_visible(popover),
                "Closing File actions must dismiss its popover");

        qualify_selection_actions(*window, popover);

        bool finalized = false;
        g_object_weak_ref(G_OBJECT(window->native_window()), [](gpointer data, GObject *) {
            *static_cast<bool *>(data) = true;
        }, &finalized);
        gtk_window_destroy(window->native_window());
        while (!finalized && g_main_context_iteration(nullptr, FALSE)) {}
        require(finalized, "Interaction window did not finalize");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        g_object_unref(application);
        return 1;
    }

    g_object_unref(application);
    return 0;
}
