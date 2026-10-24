// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/ui/file_manager_window.hpp"
#include "../src/ui/dual_pane_controller.hpp"
#include "../src/ui/clipboard_controller.hpp"
#include "../src/ui/item_operation_controller.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>

namespace infiltrator::files {
struct FileManagerWindowTestAccess {
    static void navigate(FileManagerWindow &w, const std::filesystem::path &path) {
        GFile *file = g_file_new_for_path(path.c_str());
        w.navigate_to(file, true); g_object_unref(file);
    }
    static void back(FileManagerWindow &w) { w.navigate_history(-1); }
    static bool loading(FileManagerWindow &w) { return gtk_directory_list_is_loading(w.directory_list_); }
    static bool opening(FileManagerWindow &w) { return w.location_request_ != nullptr; }
    static std::size_t count(FileManagerWindow &w) { return w.tabs_.size(); }
    static std::size_t history(FileManagerWindow &w) { return w.history_.size(); }
    static void checkpoint(FileManagerWindow &w) { w.schedule_session_checkpoint(); }
    static GtkMultiSelection *selection(FileManagerWindow &w) { return w.selection_; }
    static void icons(FileManagerWindow &w) { w.set_view_mode(FileManagerWindow::ViewMode::Icons); }
    static void choose_list(FileManagerWindow &w) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w.list_view_button_), TRUE);
    }
    static void choose_icons(FileManagerWindow &w) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w.icon_view_button_), TRUE);
    }
    static void refresh(FileManagerWindow &w) { w.refresh_current_directory(); }
    static void bookmark_action(FileManagerWindow &w, guint index, const char *verb) {
        auto *button = GTK_BUTTON(g_object_ref_sink(gtk_button_new_with_label(verb)));
        g_object_set_data(G_OBJECT(button), "ifm-bookmark-row", w.bookmarks_.at(index).row);
        FileManagerWindow::on_bookmark_action(button, &w);
        g_object_unref(button);
    }
    static bool is_icons(FileManagerWindow &w) { return w.view_mode_ == FileManagerWindow::ViewMode::Icons; }
    static void activate(FileManagerWindow &w, std::size_t i) { w.activate_tab(i); }
    static void close(FileManagerWindow &w) { w.close_tab(w.active_tab_); }
    static GFile *resolve(FileManagerWindow &w, const char *value) { return w.file_from_location_text(value); }
    static GFile *location(FileManagerWindow &w) { return w.current_location_; }
    static GtkWidget *status(FileManagerWindow &w) { return w.status_label_; }
    static GtkWidget *active_button(FileManagerWindow &w) {
        GtkWidget *group = gtk_widget_get_first_child(w.tab_strip_);
        for (std::size_t i = 0; i < w.active_tab_ && group != nullptr; ++i)
            group = gtk_widget_get_next_sibling(group);
        return group != nullptr ? gtk_widget_get_first_child(group) : nullptr;
    }
    static GtkWidget *strip(FileManagerWindow &w) { return w.tab_strip_; }
    static GType drag_type() { return FileManagerWindow::tab_drag_type(); }
    static GType bookmark_type() { return FileManagerWindow::bookmark_drag_type(); }
    static std::size_t bookmarks(FileManagerWindow &w) { return w.bookmarks_.size(); }
    static GtkWidget *bookmark_row(FileManagerWindow &w, std::size_t i) { return w.bookmarks_[i].row; }
    static std::string bookmark_label(FileManagerWindow &w, std::size_t i) { return w.bookmarks_[i].label; }
    static std::string bookmark_uri(FileManagerWindow &w, std::size_t i) { return w.bookmarks_[i].uri; }
    static void label_dialog(FileManagerWindow &w, const std::string &uri) { w.show_bookmark_label_dialog(uri); }
    static void remove_bookmark(FileManagerWindow &w, std::size_t i) { w.remove_bookmark(i); }
    static bool label(FileManagerWindow &w, const std::string &uri, const std::string &label) {
        return w.rename_bookmark(uri, label);
    }
};
}
using namespace infiltrator::files;
namespace fs = std::filesystem;
namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void await(const std::function<bool()> &ready) {
    const auto deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!ready() && g_get_monotonic_time() < deadline) {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        g_usleep(1000);
    }
    require(ready(), "Timed out waiting for workspace state");
}
bool at(FileManagerWindow &w, const fs::path &path) {
    GFile *expected = g_file_new_for_path(path.c_str());
    const bool equal = g_file_equal(FileManagerWindowTestAccess::location(w), expected);
    g_object_unref(expected); return equal;
}
void open(FileManagerWindow &w, const fs::path &path) {
    GFile *file = g_file_new_for_path(path.c_str());
    w.open_location(file); g_object_unref(file);
    await([&] { return !FileManagerWindowTestAccess::opening(w) &&
                       !FileManagerWindowTestAccess::loading(w); });
}
bool selected(FileManagerWindow &w, const char *name) {
    auto *selection = FileManagerWindowTestAccess::selection(w);
    auto *model = gtk_multi_selection_get_model(selection);
    for (guint i = 0; i < g_list_model_get_n_items(model); ++i) {
        auto *info = G_FILE_INFO(g_list_model_get_item(model, i));
        const bool match = g_strcmp0(g_file_info_get_name(info), name) == 0 &&
            gtk_selection_model_is_selected(GTK_SELECTION_MODEL(selection), i);
        g_object_unref(info);
        if (match) return true;
    }
    return false;
}
GObject *controller(GtkWidget *widget, GType type) {
    GListModel *controllers = gtk_widget_observe_controllers(widget);
    GObject *found = nullptr;
    for (guint i = 0; i < g_list_model_get_n_items(controllers); ++i) {
        auto *candidate = G_OBJECT(g_list_model_get_item(controllers, i));
        if (G_TYPE_CHECK_INSTANCE_TYPE(candidate, type)) { found = candidate; break; }
        g_object_unref(candidate);
    }
    g_object_unref(controllers);
    return found;
}
void prepare(FileManagerWindow &source, GValue &value) {
    GObject *drag = controller(FileManagerWindowTestAccess::active_button(source), GTK_TYPE_DRAG_SOURCE);
    require(drag != nullptr, "Tabs must have a real GTK drag source");
    GdkContentProvider *provider = nullptr;
    g_signal_emit_by_name(drag, "prepare", 0.0, 0.0, &provider);
    g_object_unref(drag);
    require(provider != nullptr, "Tab drag must offer an in-process payload");
    g_value_init(&value, FileManagerWindowTestAccess::drag_type());
    require(gdk_content_provider_get_value(provider, &value, nullptr), "Tab payload must round trip");
    g_object_unref(provider);
}
bool drop(FileManagerWindow &destination, const GValue &value) {
    GObject *target = controller(FileManagerWindowTestAccess::strip(destination), GTK_TYPE_DROP_TARGET);
    require(target != nullptr, "Tab rail must have a GTK drop target");
    gboolean accepted = FALSE;
    g_signal_emit_by_name(target, "drop", &value, 0.0, 0.0, &accepted);
    g_object_unref(target); return accepted;
}
FileManagerWindow *window(GtkApplication *app) {
    auto *created = new FileManagerWindow(app);
    created->present(); return created;
}
std::string file_uri(const fs::path &path) {
    GFile *file = g_file_new_for_path(path.c_str());
    char *text = g_file_get_uri(file);
    const std::string uri(text); g_free(text); g_object_unref(file); return uri;
}
GtkWidget *find(GtkWidget *root, const std::function<bool(GtkWidget *)> &matches) {
    if (root == nullptr) return nullptr;
    if (matches(root)) return root;
    for (auto *child = gtk_widget_get_first_child(root); child != nullptr;
         child = gtk_widget_get_next_sibling(child))
        if (auto *result = find(child, matches)) return result;
    return nullptr;
}
}
int main()
{
    char *temporary = g_dir_make_tmp("files-workspace-features-XXXXXX", nullptr);
    if (temporary == nullptr) return 2;
    const fs::path root(temporary); g_free(temporary);
    g_setenv("XDG_CONFIG_HOME", (root / "config").c_str(), TRUE);
    g_setenv("XDG_STATE_HOME", (root / "state").c_str(), TRUE);
    gtk_init();
    auto *app = gtk_application_new("org.infiltrator.Files.WorkspaceFeatures", G_APPLICATION_NON_UNIQUE);
    require(g_application_register(G_APPLICATION(app), nullptr, nullptr), "Cannot register application");
    try {
        const auto a = root / "a", b = root / "b";
        fs::create_directories(a / "child"); fs::create_directories(b);
        std::ofstream(a / "file with spaces.txt") << "payload";
        std::ofstream(b / "reverse-one.txt") << "from secondary one";
        std::ofstream(b / "reverse-two.txt") << "from secondary two";
        std::ofstream(a / ".hidden") << "hidden";
        const auto profile = root / "config" / "infiltrator" / "files";
        fs::create_directories(profile);
        std::ofstream(profile / "bookmarks") << file_uri(a) << '\n' << file_uri(b) << '\n';
        auto *source = window(app);
        require(FileManagerWindowTestAccess::bookmarks(*source) == 2U,
                "Legacy URI bookmarks must be imported without losing their order");
        FileManagerWindowTestAccess::label_dialog(*source, file_uri(a));
        GtkWindow *label_dialog = nullptr;
        auto *toplevels = gtk_window_get_toplevels();
        for (guint i = 0; i < g_list_model_get_n_items(toplevels); ++i) {
            auto *candidate = GTK_WINDOW(g_list_model_get_item(toplevels, i));
            if (g_strcmp0(gtk_window_get_title(candidate), "Rename Bookmark") == 0) {
                label_dialog = candidate; break;
            }
            g_object_unref(candidate);
        }
        require(label_dialog != nullptr, "Bookmark label editing must expose a real dialog");
        auto *entry = find(GTK_WIDGET(label_dialog), [](GtkWidget *w) { return GTK_IS_ENTRY(w); });
        auto *save = find(GTK_WIDGET(label_dialog), [](GtkWidget *w) {
            return GTK_IS_BUTTON(w) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(w)), "Save Label") == 0;
        });
        require(entry != nullptr && save != nullptr, "Bookmark dialog must expose its entry and save action");
        gtk_editable_set_text(GTK_EDITABLE(entry), "Project Photos");
        g_signal_emit_by_name(save, "clicked");
        g_object_unref(label_dialog);
        require(FileManagerWindowTestAccess::bookmark_label(*source, 0U) == "Project Photos" &&
                fs::exists(a / "file with spaces.txt") && fs::exists(profile / "bookmarks.ini"),
                "Renaming a bookmark must persist only its label and leave the target unchanged");
        require(!FileManagerWindowTestAccess::label(*source, file_uri(a), "bad\nlabel"),
                "Bookmark labels must refuse control characters");
        open(*source, a);
        source->install_operation_surface();
        auto *pane = static_cast<DualPaneController *>(
            g_object_get_data(G_OBJECT(source->native_window()), "ifm-dual-pane-controller"));
        require(pane != nullptr && !pane->visible(), "Dual-pane controller must start collapsed");
        pane->toggle();
        require(pane->visible() && pane->secondary_location() != nullptr,
                "Second pane must independently enumerate the current folder");
        GFile *same_folder = g_file_new_for_path(a.c_str());
        require(g_file_equal(pane->secondary_location(), same_folder),
                "Second pane must initially target the active source folder");
        g_object_unref(same_folder);
        GtkWidget *second_path = find(GTK_WIDGET(source->native_window()), [](GtkWidget *w) {
            return GTK_IS_ENTRY(w) &&
                g_strcmp0(gtk_widget_get_tooltip_text(w),
                          "Second pane location (path or URI)") == 0;
        });
        require(second_path != nullptr, "Second pane must expose editable URI navigation");
        gtk_editable_set_text(GTK_EDITABLE(second_path), b.c_str());
        g_signal_emit_by_name(second_path, "activate");
        GFile *secondary_target = g_file_new_for_path(b.c_str());
        require(g_file_equal(pane->secondary_location(), secondary_target) &&
                at(*source, a), "Second-pane navigation must not retarget the first pane");
        g_object_unref(secondary_target);
        GtkWidget *pane_host = GTK_WIDGET(g_object_get_data(
            G_OBJECT(source->native_window()), "ifm-secondary-pane-host"));
        require(pane_host != nullptr, "Second pane host must remain discoverable");
        GtkWidget *pane_list = find(pane_host, [](GtkWidget *widget) {
            return GTK_IS_LIST_VIEW(widget);
        });
        require(pane_list != nullptr &&
                GTK_IS_MULTI_SELECTION(gtk_list_view_get_model(GTK_LIST_VIEW(pane_list))),
                "Second pane must support real multi-selection");
        GObject *pane_drag = controller(pane_list, GTK_TYPE_DRAG_SOURCE);
        GObject *pane_drop = controller(pane_list, GTK_TYPE_DROP_TARGET);
        require(pane_drag != nullptr && pane_drop != nullptr,
                "Second pane must share the native drag/drop transport");
        g_clear_object(&pane_drag);
        g_clear_object(&pane_drop);
        auto *pane_selection = GTK_MULTI_SELECTION(
            gtk_list_view_get_model(GTK_LIST_VIEW(pane_list)));
        await([&] { return g_list_model_get_n_items(
            gtk_multi_selection_get_model(pane_selection)) >= 2U; });
        GtkWidget *up_in_pane = find(pane_host, [](GtkWidget *widget) {
            return GTK_IS_BUTTON(widget) && g_strcmp0(
                gtk_widget_get_tooltip_text(widget), "Second pane: Parent") == 0;
        });
        GtkWidget *back_in_pane = find(pane_host, [](GtkWidget *widget) {
            return GTK_IS_BUTTON(widget) && g_strcmp0(
                gtk_widget_get_tooltip_text(widget), "Second pane: Back") == 0;
        });
        GtkWidget *forward_in_pane = find(pane_host, [](GtkWidget *widget) {
            return GTK_IS_BUTTON(widget) && g_strcmp0(
                gtk_widget_get_tooltip_text(widget), "Second pane: Forward") == 0;
        });
        require(up_in_pane && back_in_pane && forward_in_pane,
                "Second pane must expose independent parent/back/forward navigation");
        g_signal_emit_by_name(up_in_pane, "clicked");
        GFile *root_file = g_file_new_for_path(root.c_str());
        require(g_file_equal(pane->secondary_location(), root_file) &&
                at(*source, a), "Parent navigation must affect only the second pane");
        g_object_unref(root_file);
        g_signal_emit_by_name(back_in_pane, "clicked");
        secondary_target = g_file_new_for_path(b.c_str());
        require(g_file_equal(pane->secondary_location(), secondary_target),
                "Second-pane back must restore its earlier location");
        g_signal_emit_by_name(forward_in_pane, "clicked");
        g_signal_emit_by_name(back_in_pane, "clicked");
        require(g_file_equal(pane->secondary_location(), secondary_target),
                "Second-pane forward must not corrupt back history");
        g_object_unref(secondary_target);
        GListModel *pane_source_model = gtk_multi_selection_get_model(pane_selection);
        if (GTK_IS_SORT_LIST_MODEL(pane_source_model))
            pane_source_model = gtk_sort_list_model_get_model(GTK_SORT_LIST_MODEL(pane_source_model));
        require(GTK_IS_DIRECTORY_LIST(pane_source_model),
                "Secondary sort must preserve an underlying directory enumeration");
        await([&] { return !gtk_directory_list_is_loading(GTK_DIRECTORY_LIST(pane_source_model)); });
        // Exercise the actual user-facing cross-pane action, not just the
        // second GTK directory model. It must delegate to the shared batch
        // transfer machinery and leave the source untouched for Copy.
        GtkMultiSelection *selected_in_primary = FileManagerWindowTestAccess::selection(*source);
        GListModel *selected_model = gtk_multi_selection_get_model(selected_in_primary);
        bool selected_copy_source = false;
        for (guint i = 0; i < g_list_model_get_n_items(selected_model); ++i) {
            auto *info = G_FILE_INFO(g_list_model_get_item(selected_model, i));
            if (info != nullptr &&
                g_strcmp0(g_file_info_get_name(info), "file with spaces.txt") == 0) {
                selected_copy_source = gtk_selection_model_select_item(
                    GTK_SELECTION_MODEL(selected_in_primary), i, TRUE);
            }
            g_clear_object(&info);
            if (selected_copy_source) break;
        }
        require(selected_copy_source, "Cross-pane Copy requires a real source selection");
        GtkWidget *pane_copy = find(GTK_WIDGET(source->native_window()), [](GtkWidget *widget) {
            return GTK_IS_BUTTON(widget) &&
                g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), "Copy →") == 0;
        });
        require(pane_copy != nullptr, "Second pane must expose the directional Copy operation");
        g_signal_emit_by_name(pane_copy, "clicked");
        await([&] { return fs::exists(b / "file with spaces.txt"); });
        require(fs::exists(a / "file with spaces.txt") &&
                fs::file_size(b / "file with spaces.txt") == fs::file_size(a / "file with spaces.txt"),
                "Cross-pane Copy must preserve the original and publish its contents");
        GtkWidget *select_all_in_pane = find(pane_host, [](GtkWidget *widget) {
            return GTK_IS_BUTTON(widget) &&
                g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), "Select all") == 0;
        });
        GtkWidget *copy_left = find(pane_host, [](GtkWidget *widget) {
            return GTK_IS_BUTTON(widget) &&
                g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), "← Copy") == 0;
        });
        require(select_all_in_pane && copy_left,
                "Second pane must expose multi-item reverse transfer controls");
        await([&] { return g_list_model_get_n_items(
            gtk_multi_selection_get_model(pane_selection)) >= 2U; });
        g_signal_emit_by_name(select_all_in_pane, "clicked");
        GtkBitset *all = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(pane_selection));
        require(gtk_bitset_get_size(all) ==
                    g_list_model_get_n_items(gtk_multi_selection_get_model(pane_selection)) &&
                    gtk_bitset_get_size(all) >= 2U,
                "Second-pane Select All must select every currently visible entry");
        gtk_bitset_unref(all);
        // The first forward Copy put a file in this directory that already
        // exists in the reverse destination. Only select the two new files:
        // all-or-nothing collision preflight must not be silently bypassed.
        gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(pane_selection));
        guint reverse_selected = 0;
        GListModel *reverse_model = gtk_multi_selection_get_model(pane_selection);
        for (guint i = 0; i < g_list_model_get_n_items(reverse_model); ++i) {
            auto *info = G_FILE_INFO(g_list_model_get_item(reverse_model, i));
            const char *name = info != nullptr ? g_file_info_get_name(info) : nullptr;
            if (g_strcmp0(name, "reverse-one.txt") == 0 ||
                g_strcmp0(name, "reverse-two.txt") == 0) {
                if (gtk_selection_model_select_item(GTK_SELECTION_MODEL(pane_selection), i, FALSE))
                    ++reverse_selected;
            }
            g_clear_object(&info);
        }
        require(reverse_selected == 2U,
                "Second-pane selection must identify both exact source names");
        auto *transfer_controller = static_cast<ClipboardController *>(
            g_object_get_data(G_OBJECT(source->native_window()),
                              "ifm-clipboard-controller"));
        require(transfer_controller != nullptr,
                "Cross-pane transfer must use the window's shared clipboard controller");
        await([&] { return !transfer_controller->transfer_busy(); });
        g_signal_emit_by_name(copy_left, "clicked");
        await([&] { return fs::exists(a / "reverse-one.txt") &&
                          fs::exists(a / "reverse-two.txt") &&
                          !transfer_controller->transfer_busy(); });
        require(fs::exists(b / "reverse-one.txt") && fs::exists(b / "reverse-two.txt"),
                "Reverse multi-item Copy must preserve both originals");
        pane->toggle();
        require(!pane->visible() && pane->secondary_location() == nullptr,
                "Collapsing secondary pane must relinquish its enumeration location");
        require(at(*source, a), "Opening a directory must browse it");
        GFile *relative = FileManagerWindowTestAccess::resolve(*source, "child/..");
        GFile *expected = g_file_new_for_path(a.c_str());
        require(g_file_equal(relative, expected), "Relative paths must resolve from the browsed folder");
        g_object_unref(relative); g_object_unref(expected);
        open(*source, a / "file with spaces.txt");
        require(at(*source, a) && selected(*source, "file with spaces.txt"),
                "Opening a file must reveal and select its exact filename");
        GtkWidget *permissions_action = find(GTK_WIDGET(source->native_window()),
            [](GtkWidget *widget) {
                return GTK_IS_BUTTON(widget) &&
                    g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)),
                              "Edit Permissions…") == 0;
            });
        require(permissions_action != nullptr,
                "The selected item must expose native permissions editing");
        g_signal_emit_by_name(permissions_action, "clicked");
        GtkWindow *mode_dialog = nullptr;
        auto *mode_toplevels = gtk_window_get_toplevels();
        for (guint i = 0; i < g_list_model_get_n_items(mode_toplevels); ++i) {
            auto *candidate = GTK_WINDOW(g_list_model_get_item(mode_toplevels, i));
            if (g_strcmp0(gtk_window_get_title(candidate), "Edit Permissions") == 0) {
                mode_dialog = candidate;
                break;
            }
            g_object_unref(candidate);
        }
        require(mode_dialog != nullptr,
                "Permission operation must require an explicit modal confirmation");
        auto *mode_entry = find(GTK_WIDGET(mode_dialog), [](GtkWidget *widget) {
            return GTK_IS_ENTRY(widget);
        });
        auto *apply_mode = find(GTK_WIDGET(mode_dialog), [](GtkWidget *widget) {
            return GTK_IS_BUTTON(widget) &&
                g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)),
                          "Apply Permissions") == 0;
        });
        require(mode_entry != nullptr && apply_mode != nullptr,
                "Permission dialog must expose a mode field and Apply action");
        GtkWidget *owner_execute = find(GTK_WIDGET(mode_dialog), [](GtkWidget *widget) {
            return GTK_IS_CHECK_BUTTON(widget) &&
                g_strcmp0(gtk_check_button_get_label(GTK_CHECK_BUTTON(widget)), "Execute") == 0;
        });
        require(owner_execute != nullptr, "Permission mode checkboxes must be present");
        gtk_editable_set_text(GTK_EDITABLE(mode_entry), "0600");
        require(!gtk_check_button_get_active(GTK_CHECK_BUTTON(owner_execute)),
                "Octal mode edits must update checkboxes");
        gtk_check_button_set_active(GTK_CHECK_BUTTON(owner_execute), TRUE);
        require(g_strcmp0(gtk_editable_get_text(GTK_EDITABLE(mode_entry)), "700") == 0,
                "Checkbox changes must update the octal mode field");
        gtk_editable_set_text(GTK_EDITABLE(mode_entry), "0600");
        g_signal_emit_by_name(apply_mode, "clicked");
        g_object_unref(mode_dialog);
        await([&] {
            struct stat mode_stat {};
            return ::stat((a / "file with spaces.txt").c_str(), &mode_stat) == 0 &&
                   (mode_stat.st_mode & 0777) == 0600;
        });
        auto *item_controller = static_cast<ItemOperationController *>(
            g_object_get_data(G_OBJECT(source->native_window()),
                              "ifm-item-controller"));
        require(item_controller != nullptr, "Permissions must reuse the item operation controller");
        // Mode bytes can reach the filesystem before the GTask completion and
        // directory monitor are processed. Do not navigate while that earlier
        // mutation still owns its window/monitor lease.
        await([&] { return !item_controller->operation_busy(); });

        open(*source, a / ".hidden");
        require(selected(*source, ".hidden") && !source->show_hidden(),
                "Explicitly opened hidden files must be revealed without changing the saved preference");
        open(*source, root / "missing");
        require(at(*source, a) && std::string(gtk_label_get_text(GTK_LABEL(
                    FileManagerWindowTestAccess::status(*source)))).find("could not be opened") != std::string::npos,
                "An invalid open request must retain the previous location and explain failure");
        GFile *superseded = g_file_new_for_path(a.c_str());
        source->open_location(superseded); g_object_unref(superseded);
        FileManagerWindowTestAccess::navigate(*source, b);
        await([&] { return !FileManagerWindowTestAccess::loading(*source); });
        while (g_main_context_iteration(nullptr, FALSE)) {}
        require(at(*source, b), "A superseded metadata callback must never retarget navigation");
        open(*source, a / "file with spaces.txt");
        FileManagerWindowTestAccess::icons(*source);
        const auto history = FileManagerWindowTestAccess::history(*source);
        source->duplicate_active_tab();
        require(FileManagerWindowTestAccess::count(*source) == 2U &&
                FileManagerWindowTestAccess::history(*source) == history &&
                selected(*source, "file with spaces.txt"),
                "Duplicate Tab must preserve history, selection and the current history index");
        auto *destination = window(app);
        require(FileManagerWindowTestAccess::bookmark_label(*destination, 0U) == "Project Photos",
                "Bookmark labels must survive creating a new window");
        GValue bookmark_payload = G_VALUE_INIT;
        auto *bookmark_drag = controller(FileManagerWindowTestAccess::bookmark_row(*source, 0U), GTK_TYPE_DRAG_SOURCE);
        require(bookmark_drag != nullptr, "Bookmarks must offer a drag source");
        GdkContentProvider *bookmark_provider = nullptr;
        g_signal_emit_by_name(bookmark_drag, "prepare", 0.0, 0.0, &bookmark_provider);
        g_object_unref(bookmark_drag);
        g_value_init(&bookmark_payload, FileManagerWindowTestAccess::bookmark_type());
        require(bookmark_provider != nullptr && gdk_content_provider_get_value(bookmark_provider, &bookmark_payload, nullptr),
                "Bookmark drag must offer an internal, lifetime-bound payload");
        g_object_unref(bookmark_provider);
        auto *bookmark_target = controller(FileManagerWindowTestAccess::bookmark_row(*source, 1U), GTK_TYPE_DROP_TARGET);
        require(bookmark_target != nullptr, "Bookmarks must expose reorder drop targets");
        gboolean reordered = FALSE;
        g_signal_emit_by_name(bookmark_target, "drop", &bookmark_payload, 0.0, 1000.0, &reordered);
        g_object_unref(bookmark_target);
        require(reordered && FileManagerWindowTestAccess::bookmark_uri(*source, 1U) == file_uri(a) &&
                FileManagerWindowTestAccess::bookmark_uri(*destination, 1U) == file_uri(a) &&
                FileManagerWindowTestAccess::bookmark_label(*destination, 1U) == "Project Photos",
                "Drag reorder must persist labels and update the other open windows");
        FileManagerWindowTestAccess::remove_bookmark(*source, 1U);
        bookmark_target = controller(FileManagerWindowTestAccess::bookmark_row(*source, 0U), GTK_TYPE_DROP_TARGET);
        reordered = FALSE;
        g_signal_emit_by_name(bookmark_target, "drop", &bookmark_payload, 0.0, 0.0, &reordered);
        g_object_unref(bookmark_target);
        require(!reordered && FileManagerWindowTestAccess::bookmarks(*destination) == 1U,
                "Removing a bookmark must revoke an old drag and update every window");
        g_value_unset(&bookmark_payload);
        GValue payload = G_VALUE_INIT;
        prepare(*source, payload);
        require(drop(*destination, payload), "A tab must move between independent application windows");
        g_value_unset(&payload);
        await([&] { return !FileManagerWindowTestAccess::loading(*destination); });
        require(FileManagerWindowTestAccess::count(*source) == 1U &&
                FileManagerWindowTestAccess::count(*destination) == 2U && at(*destination, a) &&
                selected(*destination, "file with spaces.txt") &&
                FileManagerWindowTestAccess::is_icons(*destination) &&
                FileManagerWindowTestAccess::history(*destination) == history,
                "Moving a tab must retain its full browsing state and leave the other tabs intact");
        FileManagerWindowTestAccess::back(*destination);
        require(at(*destination, b) && at(*source, a), "Moved and original histories must be independent");

        // Close/reopen is deliberately a new identity: an old drag is no
        // longer authorised to move the reopened tab at the same index.
        prepare(*destination, payload);
        FileManagerWindowTestAccess::close(*destination);
        destination->reopen_closed_tab();
        require(!drop(*source, payload), "A stale drag must not move a subsequently reopened tab");
        g_value_unset(&payload);
        prepare(*source, payload);
        gtk_window_destroy(source->native_window());
        require(!drop(*destination, payload), "A closed source window must revoke a pending drag");
        g_value_unset(&payload);

        auto *last = window(app);
        GFile *pending = g_file_new_for_path(a.c_str());
        last->open_location(pending); g_object_unref(pending);
        gtk_window_destroy(last->native_window());
        while (g_main_context_iteration(nullptr, FALSE)) {}
        open(*destination, a / "file with spaces.txt");
        const auto saved_history = FileManagerWindowTestAccess::history(*destination);
        gtk_window_destroy(destination->native_window());
        while (g_main_context_iteration(nullptr, FALSE)) {}
        const auto session_path = root / "config" / "infiltrator" / "files" / "session.ini";
        require(fs::exists(session_path), "Closing the last window must persist browsing state");
        auto *restored = window(app);
        require(restored->restore_previous_session(), "A normal close must yield a restorable session");
        await([&] { return !FileManagerWindowTestAccess::loading(*restored); });
        require(FileManagerWindowTestAccess::count(*restored) == 2U && at(*restored, a) &&
                selected(*restored, "file with spaces.txt") && FileManagerWindowTestAccess::is_icons(*restored) &&
                FileManagerWindowTestAccess::history(*restored) == saved_history,
                "Restart must restore tabs, active location, selection, view and navigation history");
        std::ofstream(session_path, std::ios::binary | std::ios::trunc) << "[Session]\nversion=999\n";
        require(!restored->restore_previous_session() && at(*restored, a) &&
                FileManagerWindowTestAccess::count(*restored) == 2U,
                "Malformed session state must not partially replace the current workspace");
        std::ofstream(session_path, std::ios::binary | std::ios::trunc) << std::string(1024U * 1024U + 1U, 'x');
        require(!restored->restore_previous_session() && at(*restored, a),
                "Oversized session files must be refused before parsing");
        // Remember a folder's explicit view choice across navigation and an
        // independently created window. Programmatic restore must not mutate it.
        open(*restored, a);
        FileManagerWindowTestAccess::choose_list(*restored);
        FileManagerWindowTestAccess::choose_icons(*restored);
        open(*restored, b);
        FileManagerWindowTestAccess::choose_list(*restored);
        open(*restored, a);
        require(FileManagerWindowTestAccess::is_icons(*restored),
                "Explicit folder view must survive navigation to another location");
        auto *another = window(app);
        open(*another, a);
        require(FileManagerWindowTestAccess::is_icons(*another),
                "Folder view must load from durable state in another window");
        open(*another, b);
        require(!FileManagerWindowTestAccess::is_icons(*another),
                "Independent folder view choice must survive across windows");
        gtk_window_destroy(another->native_window());

        // Refresh must not introduce a history step or lose a selected file.
        open(*restored, a / "file with spaces.txt");
        const auto before_refresh = FileManagerWindowTestAccess::history(*restored);
        FileManagerWindowTestAccess::refresh(*restored);
        await([&] { return !FileManagerWindowTestAccess::loading(*restored); });
        require(FileManagerWindowTestAccess::history(*restored) == before_refresh &&
                selected(*restored, "file with spaces.txt"),
                "Refresh must re-enumerate without replacing navigation or selection");

        require(FileManagerWindowTestAccess::bookmarks(*restored) == 1U,
                "The saved bookmark must be available to workspace actions");
        const auto tabs_before = FileManagerWindowTestAccess::count(*restored);
        FileManagerWindowTestAccess::bookmark_action(*restored, 0U, "Open in new tab");
        await([&] { return !FileManagerWindowTestAccess::loading(*restored); });
        require(FileManagerWindowTestAccess::count(*restored) == tabs_before + 1U &&
                at(*restored, b),
                "Bookmark new-tab action must navigate independently of the prior tab");
        const guint windows_before = g_list_length(gtk_application_get_windows(app));
        FileManagerWindowTestAccess::bookmark_action(*restored, 0U, "Open in new window");
        require(g_list_length(gtk_application_get_windows(app)) == windows_before + 1U,
                "Bookmark new-window action must create an application window");
        for (GList *node = gtk_application_get_windows(app); node != nullptr; node = node->next) {
            if (node->data != restored->native_window()) {
                gtk_window_destroy(GTK_WINDOW(node->data));
                break;
            }
        }
        // Invalid saved preferences must fail closed, never replace the file.
        const auto views_path = profile / "folder-views.ini";
        std::ofstream(views_path, std::ios::binary | std::ios::trunc) << "invalid folder views";
        FileManagerWindowTestAccess::choose_icons(*restored);
        std::ifstream views(views_path);
        std::string preserved;
        std::getline(views, preserved);
        require(preserved == "invalid folder views",
                "Malformed per-folder views must not be overwritten");
        gtk_window_destroy(restored->native_window());
        while (g_main_context_iteration(nullptr, FALSE)) {}
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
    g_object_unref(app); fs::remove_all(root); return 0;
}
