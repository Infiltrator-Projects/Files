// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/ui/file_manager_window.hpp"

#include <gtk/gtk.h>

#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
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

GtkWidget *find_expander(GtkWidget *root, const char *label)
{
    if (root == nullptr) return nullptr;
    if (GTK_IS_EXPANDER(root) &&
        g_strcmp0(gtk_expander_get_label(GTK_EXPANDER(root)), label) == 0)
        return root;
    for (GtkWidget *child = gtk_widget_get_first_child(root); child != nullptr;
         child = gtk_widget_get_next_sibling(child)) {
        if (GtkWidget *result = find_expander(child, label); result != nullptr)
            return result;
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



GtkWindow *window_named(const char *title)
{
    GList *windows = gtk_window_list_toplevels();
    GtkWindow *found = nullptr;
    for (GList *it = windows; it != nullptr; it = it->next) {
        GtkWindow *window = GTK_WINDOW(it->data);
        if (g_strcmp0(gtk_window_get_title(window), title) == 0) {
            found = window;
            break;
        }
    }
    g_list_free(windows);
    return found;
}

std::string text_in(GtkWidget *root)
{
    if (root == nullptr) return {};
    if (GTK_IS_TEXT_VIEW(root)) {
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(root));
        GtkTextIter begin, end;
        gtk_text_buffer_get_bounds(buffer, &begin, &end);
        char *value = gtk_text_buffer_get_text(buffer, &begin, &end, FALSE);
        std::string result = value != nullptr ? value : "";
        g_free(value);
        return result;
    }
    for (GtkWidget *child = gtk_widget_get_first_child(root); child != nullptr;
         child = gtk_widget_get_next_sibling(child)) {
        const std::string result = text_in(child);
        if (!result.empty()) return result;
    }
    return {};
}

bool await_text(GtkWidget *root, const std::string &needle)
{
    for (int i = 0; i < 5000; ++i) {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        if (text_in(root).find(needle) != std::string::npos) return true;
        g_usleep(1000);
    }
    return false;
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

GtkWidget *find_entry_by_placeholder(GtkWidget *root, const char *placeholder)
{
    if (root == nullptr) return nullptr;
    if (GTK_IS_ENTRY(root) &&
        g_strcmp0(gtk_entry_get_placeholder_text(GTK_ENTRY(root)), placeholder) == 0)
        return root;
    for (GtkWidget *child = gtk_widget_get_first_child(root); child != nullptr;
         child = gtk_widget_get_next_sibling(child)) {
        if (GtkWidget *found = find_entry_by_placeholder(child, placeholder); found != nullptr)
            return found;
    }
    return nullptr;
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

std::vector<GtkWidget *> find_entries(GtkWidget *root)
{
    std::vector<GtkWidget *> result;
    const auto walk = [&](auto &&self, GtkWidget *widget) -> void {
        if (GTK_IS_ENTRY(widget)) result.push_back(widget);
        for (GtkWidget *child = gtk_widget_get_first_child(widget); child != nullptr;
             child = gtk_widget_get_next_sibling(child)) self(self, child);
    };
    walk(walk, root);
    return result;
}

GtkWindow *pattern_dialog()
{
    GList *top = gtk_window_list_toplevels();
    GtkWindow *result = nullptr;
    for (GList *it = top; it != nullptr; it = it->next) {
        GtkWindow *win = GTK_WINDOW(it->data);
        if (g_strcmp0(gtk_window_get_title(win), "Select by pattern, size or date") == 0) {
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
    GdkPixbuf *small_image = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 4, 3);
    require(small_image != nullptr, "Failed to create image preview fixture");
    gdk_pixbuf_fill(small_image, 0x336699ffU);
    GError *image_error = nullptr;
    require(gdk_pixbuf_save(small_image, (root / "photo.png").c_str(),
                            "png", &image_error, nullptr),
            "Failed to save image preview fixture");
    g_object_unref(small_image);
    g_clear_error(&image_error);
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
                 g_list_model_get_n_items(items) == 6U;
        if (!loaded) g_usleep(1000);
    }
    require(loaded, "Test location did not enumerate six files/folders");
    GtkWidget *context = GTK_WIDGET(g_object_get_data(
        G_OBJECT(w.native_window()), "ifm-context-popover"));
    require(GTK_IS_POPOVER(context), "Context menu must exist at selection test");
    require(!gtk_widget_get_visible(find_button(context, "Restore")),
            "Regular filesystem locations must not expose Restore");

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

    // Complete the original catalogue's pattern/type/date/size selection
    // family; these are one requirement, not multiple invented features.
    g_signal_emit_by_name(pattern_action, "clicked");
    dialog = pattern_dialog();
    require(dialog != nullptr, "Metadata selection dialog must be available");
    auto fields = find_entries(GTK_WIDGET(dialog));
    require(fields.size() == 5U, "Selection dialog must expose five criteria");
    gtk_editable_set_text(GTK_EDITABLE(fields[1]), "10");
    apply = find_button(GTK_WIDGET(dialog), "Select matches");
    require(apply != nullptr, "Metadata selection apply action missing");
    g_signal_emit_by_name(apply, "clicked");
    require(selected_count(selection) == 1U &&
            gtk_selection_model_is_selected(GTK_SELECTION_MODEL(selection),
                                            position_for(selection, "photo.png")),
            "Size selection must match only PNG, not PDF or text fixtures");

    g_signal_emit_by_name(pattern_action, "clicked");
    dialog = pattern_dialog();
    require(dialog != nullptr, "Date selection dialog did not open");
    fields = find_entries(GTK_WIDGET(dialog));
    require(fields.size() == 5U, "Date filter fields missing");
    gtk_editable_set_text(GTK_EDITABLE(fields[3]), "2099-01-01");
    apply = find_button(GTK_WIDGET(dialog), "Select matches");
    g_signal_emit_by_name(apply, "clicked");
    require(selected_count(selection) == 0U,
            "Future modified-since bound must exclude all fixture files");

    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(selection));
    gtk_selection_model_select_item(GTK_SELECTION_MODEL(selection),
                                    position_for(selection, "first.pdf"), TRUE);
    require(!gtk_widget_get_visible(find_button(context, "New Folder")),
            "Item context menu must not include background creation controls");
    require(gtk_widget_get_visible(find_button(context, "Open With…")),
            "Regular file context menu must offer Open With");
    require(g_object_get_data(G_OBJECT(find_button(context, "Copy")),
                              "ifm-proxy-target") == find_button(popover, "Copy"),
            "Item context Copy must delegate to the installed clipboard action");
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


    // Read-only inspection actions must retain the exact source selection.
    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(selection));
    gtk_selection_model_select_item(GTK_SELECTION_MODEL(selection),
                                    position_for(selection, "first.pdf"), TRUE);
    require(gtk_widget_get_visible(find_button(context, "Inspect file…")),
            "Regular file must expose inspection without expanding the context menu");
    g_signal_emit_by_name(find_button(context, "Inspect file…"), "clicked");
    GtkWindow *inspection = window_named("Inspect file");
    require(inspection != nullptr, "Inspection panel did not open");
    require(await_text(GTK_WIDGET(inspection), "Content type"),
            "Inspection metadata did not load asynchronously");
    GtkWidget *hash = find_button(GTK_WIDGET(inspection), "SHA-256");
    require(hash != nullptr, "Regular-file SHA-256 action is missing");
    g_signal_emit_by_name(hash, "clicked");
    require(await_text(GTK_WIDGET(inspection), "SHA-256:"),
            "SHA-256 worker failed or did not report its result");
    GtkWidget *verify = find_button(GTK_WIDGET(inspection), "Verify SHA-256");
    require(verify != nullptr, "The catalogue's external checksum verification is missing");
    GtkWidget *input = find_entry(GTK_WIDGET(inspection));
    require(input != nullptr, "SHA-256 verification must accept a supplied digest");
    gtk_editable_set_text(GTK_EDITABLE(input),
        "5cf9a9f38be47af7123f6f8f1ef8e22682262a83991de5e932967857e322a188");
    g_signal_emit_by_name(verify, "clicked");
    require(await_text(GTK_WIDGET(inspection), "SHA-256 VERIFIED"),
            "Known external SHA-256 should verify the test file");
    gtk_editable_set_text(GTK_EDITABLE(input),
        "5cf9a9f38be47af7123f6f8f1ef8e22682262a83991de5e932967857e322a188 *first.pdf");
    g_signal_emit_by_name(verify, "clicked");
    require(await_text(GTK_WIDGET(inspection), "SHA-256 VERIFIED"),
            "Qualified sha256sum line must verify the selected basename");
    gtk_editable_set_text(GTK_EDITABLE(input),
        "5cf9a9f38be47af7123f6f8f1ef8e22682262a83991de5e932967857e322a188 *other.pdf");
    g_signal_emit_by_name(verify, "clicked");
    require(await_text(GTK_WIDGET(inspection), "exact filename"),
            "Manifest for a different filename must never be accepted");
    gtk_editable_set_text(GTK_EDITABLE(input),
        "0000000000000000000000000000000000000000000000000000000000000000");
    g_signal_emit_by_name(verify, "clicked");
    require(await_text(GTK_WIDGET(inspection), "SHA-256 MISMATCH"),
            "Wrong external SHA-256 must produce an unmistakable mismatch");
    GtkWidget *copy_details = find_button(GTK_WIDGET(inspection), "Copy details");
    require(copy_details != nullptr, "Copy details action missing");
    g_signal_emit_by_name(copy_details, "clicked");
    require(clipboard_text(w.native_window()).find("SHA-256:") != std::string::npos,
            "Copy details must export the visible report");
    gtk_window_destroy(inspection);

    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(selection));
    gtk_selection_model_select_item(GTK_SELECTION_MODEL(selection),
                                    position_for(selection, "first.pdf"), TRUE);
    gtk_selection_model_select_item(GTK_SELECTION_MODEL(selection),
                                    position_for(selection, "second.pdf"), FALSE);
    GtkWidget *compare = find_button(context, "Compare two file hashes…");
    require(compare != nullptr && gtk_widget_get_visible(compare),
            "Two regular files must expose hash comparison");
    g_signal_emit_by_name(compare, "clicked");
    GtkWindow *comparison = window_named("Compare file hashes");
    require(comparison != nullptr, "Two-file comparison did not open");
    require(await_text(GTK_WIDGET(comparison), "Different SHA-256 digests."),
            "Comparison must report distinct test contents");
    gtk_window_destroy(comparison);

    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(selection));
    gtk_selection_model_select_item(GTK_SELECTION_MODEL(selection),
                                    position_for(selection, "alpha"), TRUE);
    require(gtk_widget_get_visible(find_button(context, "Open folder in terminal")),
            "Native folder must expose terminal launch");
    g_signal_emit_by_name(find_button(context, "Inspect file…"), "clicked");
    inspection = window_named("Inspect file");
    require(inspection != nullptr, "Folder inspection did not open");
    GtkWidget *folder_size = find_button(GTK_WIDGET(inspection), "Calculate folder size");
    require(folder_size != nullptr, "Folder size action is missing");
    require(await_text(GTK_WIDGET(inspection), "Content type"),
            "Folder metadata did not finish");
    g_signal_emit_by_name(folder_size, "clicked");
    require(await_text(GTK_WIDGET(inspection), "Items scanned: 0"),
            "Cancellable recursive folder scan did not report its result");
    require(text_in(GTK_WIDGET(inspection)).find("Allocated total:") != std::string::npos,
            "Folder scan must distinguish allocated and logical sizes");
    gtk_window_destroy(inspection);


    // Quick Look consumes a snapshot of the visible directory model, not a
    // mutable row pointer: switching files must not change the source model.
    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(selection));
    gtk_selection_model_select_item(GTK_SELECTION_MODEL(selection),
                                    position_for(selection, "notes.txt"), TRUE);
    GtkWidget *quick = find_button(context, "Quick Look");
    require(quick != nullptr && gtk_widget_get_visible(quick),
            "Regular selection must expose a compact Quick Look action");
    g_signal_emit_by_name(quick, "clicked");
    GtkWindow *look = window_named("Quick Look");
    require(look != nullptr, "Quick Look dialog did not open");
    require(await_text(GTK_WIDGET(look), "notes"),
            "UTF-8 text preview did not render");
    GtkWidget *copy_text = find_button(GTK_WIDGET(look), "Copy text");
    require(copy_text != nullptr && gtk_widget_get_sensitive(copy_text),
            "Text preview must allow copying the visible contents");
    g_signal_emit_by_name(copy_text, "clicked");
    require(clipboard_text(w.native_window()) == "notes",
            "Quick Look text-copy must preserve the visible text");
    GtkWidget *copy_path = find_button(GTK_WIDGET(look), "Copy path");
    require(copy_path != nullptr, "Quick Look path-copy missing");
    g_signal_emit_by_name(copy_path, "clicked");
    require(clipboard_text(w.native_window()) == (root / "notes.txt").string(),
            "Quick Look must copy the exact file path");
    GtkWidget *next = find_button(GTK_WIDGET(look), "Next");
    GtkWidget *previous = find_button(GTK_WIDGET(look), "Previous");
    require(next != nullptr && previous != nullptr,
            "Quick Look must expose adjacent-file navigation");
    const bool can_next = gtk_widget_get_sensitive(next);
    const bool can_prev = gtk_widget_get_sensitive(previous);
    require(can_next || can_prev,
            "A populated folder must allow movement to another preview");
    g_signal_emit_by_name(can_next ? next : previous, "clicked");
    require(await_text(GTK_WIDGET(look), "preview") || window_named("Quick Look") != nullptr,
            "Neighbour preview navigation must retain a valid window");
    gtk_window_destroy(look);

    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(selection));
    gtk_selection_model_select_item(GTK_SELECTION_MODEL(selection),
                                    position_for(selection, "photo.png"), TRUE);
    g_signal_emit_by_name(quick, "clicked");
    look = window_named("Quick Look");
    require(look != nullptr, "Image preview dialog did not open");
    GtkWidget *zoom_in = find_button(GTK_WIDGET(look), "Zoom +");
    GtkWidget *zoom_out = find_button(GTK_WIDGET(look), "Zoom −");
    require(zoom_in != nullptr && zoom_out != nullptr,
            "Image preview must expose bounded zoom controls");
    bool image_loaded = false;
    for (int n = 0; n < 5000 && !image_loaded; ++n) {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        image_loaded = gtk_widget_get_sensitive(zoom_in);
        if (!image_loaded) g_usleep(1000);
    }
    require(image_loaded, "Image decoding must complete off-thread and enable zoom");
    g_signal_emit_by_name(zoom_in, "clicked");
    require(gtk_widget_get_sensitive(zoom_out),
            "Zooming in must make zoom out available");
    gtk_window_destroy(look);

    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(selection));
    gtk_selection_model_select_item(GTK_SELECTION_MODEL(selection),
                                    position_for(selection, "first.pdf"), TRUE);
    g_signal_emit_by_name(quick, "clicked");
    look = window_named("Quick Look");
    require(look != nullptr && await_text(GTK_WIDGET(look), "Binary preview"),
            "Unsupported binary format must show bounded hex, never launch another app");
    gtk_window_destroy(look);

    // Catalogue #357: search can span a user-selected set of locations
    // without accidentally widening scope to the entire containing folder.
    { std::ofstream(root / "alpha" / "alpha-found.txt") << "alpha"; }
    { std::ofstream(root / "beta" / "beta-found.txt") << "beta"; }
    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(selection));
    gtk_selection_model_select_item(GTK_SELECTION_MODEL(selection),
                                    position_for(selection, "alpha"), TRUE);
    gtk_selection_model_select_item(GTK_SELECTION_MODEL(selection),
                                    position_for(selection, "beta"), FALSE);
    GtkWidget *selected_search = find_button(popover, "Search selected folders…");
    require(selected_search != nullptr, "Multi-folder search command missing");
    g_signal_emit_by_name(selected_search, "clicked");
    GtkWindow *scoped = window_named("Search files");
    require(scoped != nullptr, "Selected-directory search did not open");
    GtkWidget *name_query = find_entry(GTK_WIDGET(scoped));
    require(name_query != nullptr, "Selected-directory search needs a query field");
    gtk_editable_set_text(GTK_EDITABLE(name_query), "found.txt");
    GtkWidget *search_now = find_button(GTK_WIDGET(scoped), "Search");
    require(search_now != nullptr, "Selected-directory search action missing");
    g_signal_emit_by_name(search_now, "clicked");
    GtkWidget *scoped_results = GTK_WIDGET(g_object_get_data(
        G_OBJECT(scoped), "ifm-recursive-results"));
    bool scoped_complete = false;
    for (int i = 0; i < 5000 && !scoped_complete; ++i) {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        scoped_complete = gtk_list_box_get_row_at_index(GTK_LIST_BOX(scoped_results), 1) != nullptr;
        if (!scoped_complete) g_usleep(1000);
    }
    require(scoped_complete &&
            gtk_list_box_get_row_at_index(GTK_LIST_BOX(scoped_results), 2) == nullptr,
            "Exactly two selected folders must contribute results");
    gtk_window_destroy(scoped);

    const std::size_t before = infiltrator::files::FileManagerWindowTestAccess::tabs(w);
    g_signal_emit_by_name(tabs_action, "clicked");
    require(infiltrator::files::FileManagerWindowTestAccess::tabs(w) == before + 2U,
            "Opening selected folders must create exactly two tabs");
    while (g_main_context_iteration(nullptr, FALSE)) {}
    fs::remove_all(root);
}


void qualify_recursive_search(infiltrator::files::FileManagerWindow &window,
                              GtkWidget *popover)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("files-search-ui-" + std::to_string(g_get_monotonic_time()));
    fs::create_directories(root / "nested");
    { std::ofstream(root / "nested" / "target.txt") << "unique search content"; }
    { std::ofstream(root / "unrelated.txt") << "other"; }
    GFile *folder = g_file_new_for_path(root.c_str());
    window.open_location(folder);
    auto *listing = infiltrator::files::FileManagerWindowTestAccess::listing(window);
    bool open = false;
    for (int i = 0; i < 5000 && !open; ++i) {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        GFile *current = gtk_directory_list_get_file(listing);
        open = current != nullptr && g_file_equal(current, folder) &&
               !gtk_directory_list_is_loading(listing);
        if (!open) g_usleep(1000);
    }
    require(open, "Search UI fixture folder failed to open");
    GtkWidget *action = find_button(popover, "Search in subfolders…");
    require(action != nullptr, "Recursive search must be exposed in File actions");
    g_signal_emit_by_name(action, "clicked");
    GtkWindow *dialog = window_named("Search files");
    require(dialog != nullptr, "Recursive search dialog did not open");
    GtkWidget *name = find_entry(GTK_WIDGET(dialog));
    require(GTK_IS_EDITABLE(name), "Search filename input missing");
    gtk_editable_set_text(GTK_EDITABLE(name), "target.txt");
    GtkWidget *start = find_button(GTK_WIDGET(dialog), "Search");
    require(start != nullptr, "Search launch control missing");
    g_signal_emit_by_name(start, "clicked");
    GtkWidget *results = GTK_WIDGET(g_object_get_data(
        G_OBJECT(dialog), "ifm-recursive-results"));
    require(GTK_IS_LIST_BOX(results), "Search result list missing");
    GtkListBoxRow *hit = nullptr;
    for (int n = 0; n < 5000 && hit == nullptr; ++n) {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        hit = gtk_list_box_get_row_at_index(GTK_LIST_BOX(results), 0);
        if (hit == nullptr) g_usleep(1000);
    }
    require(hit != nullptr &&
            gtk_list_box_get_row_at_index(GTK_LIST_BOX(results), 1) == nullptr,
            "Recursive filename query must produce exactly one result");
    // Catalogue #365: a query is a durable named preset rather than a
    // transient field value or a fabricated filesystem location.
    GtkWidget *preset_name = find_entry_by_placeholder(
        GTK_WIDGET(dialog), "Saved search name");
    GtkWidget *filename = find_entry_by_placeholder(
        GTK_WIDGET(dialog), "Part of name, exact name or *.pdf");
    require(preset_name != nullptr && filename != nullptr,
            "Named search and query controls must both be available");
    gtk_editable_set_text(GTK_EDITABLE(preset_name), "Fixture Search");
    GtkWidget *save = find_button(GTK_WIDGET(dialog), "Save Search");
    require(save != nullptr, "Named search save action missing");
    g_signal_emit_by_name(save, "clicked");
    gtk_editable_set_text(GTK_EDITABLE(filename), "not-the-right-query");
    GtkWidget *load = find_button(GTK_WIDGET(dialog), "Load saved");
    require(load != nullptr, "Named search load action missing");
    g_signal_emit_by_name(load, "clicked");
    require(g_strcmp0(gtk_editable_get_text(GTK_EDITABLE(filename)),
                      "target.txt") == 0,
            "Loading a saved search must restore original query values");
    gtk_list_box_select_row(GTK_LIST_BOX(results), hit);
    GtkWidget *reveal = find_button(GTK_WIDGET(dialog), "Reveal in containing folder");
    require(reveal != nullptr && gtk_widget_get_sensitive(reveal),
            "Search result reveal action must be available");
    g_signal_emit_by_name(reveal, "clicked");
    GFile *nested = g_file_get_child(folder, "nested");
    bool revealed = false;
    for (int n = 0; n < 5000 && !revealed; ++n) {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        GFile *current = gtk_directory_list_get_file(listing);
        if (current != nullptr && g_file_equal(current, nested) &&
            !gtk_directory_list_is_loading(listing)) {
            auto *selection = infiltrator::files::FileManagerWindowTestAccess::selection(window);
            if (gtk_multi_selection_get_model(selection) != nullptr) {
                const guint selected = selected_count(selection);
                if (selected == 1U) {
                    const guint pos = position_for(selection, "target.txt");
                    revealed = gtk_selection_model_is_selected(GTK_SELECTION_MODEL(selection), pos);
                }
            }
        }
        if (!revealed) g_usleep(1000);
    }
    require(revealed, "Reveal must navigate to the parent and select the original file");
    g_object_unref(nested);
    g_object_unref(folder);
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
        GtkWidget *inspection_tools = find_expander(popover, "Inspection and tools");
        require(inspection_tools != nullptr, "Inspection tools section is missing");
        gtk_expander_set_expanded(GTK_EXPANDER(inspection_tools), TRUE);
        while (g_main_context_iteration(nullptr, FALSE)) {}
        require(find_button(popover, "Open terminal here") != nullptr,
                "Current-folder terminal action must be reachable in expanded tools");
        require(find_button(popover, "Inspect file…") != nullptr,
                "File inspection must be reachable in overflow");
        require(find_button(popover, "Copy") != nullptr,
                "Clipboard Copy must be available from File actions");
        require(find_button(popover, "Cut") != nullptr,
                "Clipboard Cut must be available from File actions");
        require(find_button(popover, "Paste") != nullptr,
                "Clipboard Paste must be available from File actions");

        auto *context = GTK_WIDGET(g_object_get_data(
            G_OBJECT(window->native_window()), "ifm-context-popover"));
        require(GTK_IS_POPOVER(context), "Native right-click popover is missing");
        GtkWidget *context_scroller =
            gtk_popover_get_child(GTK_POPOVER(context));
        require(GTK_IS_SCROLLED_WINDOW(context_scroller),
                "Right-click menu must be scrollable rather than unbounded");
        require(gtk_scrolled_window_get_max_content_height(
                    GTK_SCROLLED_WINDOW(context_scroller)) <= 350,
                "Right-click menu must fit on compact screens");
        require(find_button(context, "New Folder") != nullptr &&
                find_button(context, "Paste") != nullptr &&
                find_button(context, "Restore") != nullptr,
                "Right-click actions must retain creation, clipboard and recovery");
        require(gtk_widget_get_visible(find_button(context, "New Folder")),
                "Empty-directory right click must offer folder creation");
        require(!gtk_widget_get_visible(find_button(context, "Restore")),
                "Restore must not appear outside Trash");
        gtk_menu_button_popdown(GTK_MENU_BUTTON(overflow));
        require(!gtk_widget_get_visible(popover),
                "Closing File actions must dismiss its popover");

        qualify_selection_actions(*window, popover);

        GtkWidget *layout = find_expander(popover, "View and layout");
        require(layout != nullptr, "View and layout menu is missing");
        gtk_expander_set_expanded(GTK_EXPANDER(layout), TRUE);
        while (g_main_context_iteration(nullptr, FALSE)) {}
        GtkWidget *hide_extensions = find_button(popover, "Hide filename extensions");
        require(hide_extensions != nullptr && window->show_extensions(),
                "Default presentation must show real filename extensions");
        auto *selection = infiltrator::files::FileManagerWindowTestAccess::selection(*window);
        const auto before_selection = selected_count(selection);
        g_signal_emit_by_name(hide_extensions, "clicked");
        require(!window->show_extensions(), "Extension visibility command must apply");
        require(selected_count(selection) == before_selection,
                "Presentation choice may not change the authoritative selection");
        GtkWidget *show_extensions = find_button(popover, "Show filename extensions");
        require(show_extensions != nullptr, "Extension toggle must reflect current policy");
        g_signal_emit_by_name(show_extensions, "clicked");
        require(window->show_extensions(), "Visible file extensions must be restorable");

        qualify_recursive_search(*window, popover);

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
