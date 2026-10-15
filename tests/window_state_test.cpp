// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/ui/file_manager_window.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace infiltrator::files {

// Exercise the actual GTK state transitions without depending on real hardware
// hotplug or changing the workstation's mounts/preferences.
struct FileManagerWindowTestAccess {
    static void mounts(FileManagerWindow &window, std::vector<MountedPlace> places)
    {
        window.apply_mounted_places(normalize_mounted_places(std::move(places)));
    }
    static void navigate(FileManagerWindow &window, const std::filesystem::path &path)
    {
        GFile *file = g_file_new_for_path(path.c_str());
        window.navigate_to(file, true);
        g_object_unref(file);
    }
    static bool available(const FileManagerWindow &window)
    {
        return window.current_location_available_;
    }
    static GtkWidget *row(const FileManagerWindow &window, const std::string &target)
    {
        for (const auto &entry : window.mounted_place_rows_) {
            if (entry.place.uri == target) {
                return entry.row;
            }
        }
        return nullptr;
    }
    static std::size_t row_count(const FileManagerWindow &window)
    {
        return window.mounted_place_rows_.size();
    }
    static GtkDirectoryList *directory(FileManagerWindow &window) { return window.directory_list_; }
    static GtkMultiSelection *selection(FileManagerWindow &window) { return window.selection_; }
    static GtkSingleSelection *primary(FileManagerWindow &window) { return window.primary_selection_; }
    static bool watching_policy(const FileManagerWindow &window)
    {
        return window.temporal_policy_monitor_ != nullptr && !window.temporal_policy_monitoring_parent_;
    }
    static std::string watch_name(const FileManagerWindow &window)
    {
        return window.temporal_policy_watch_name_;
    }
    static GtkLabel *status(FileManagerWindow &window) { return GTK_LABEL(window.status_label_); }
    static void finish_launch(GObject *source, GAsyncResult *result, GtkLabel *status)
    {
        FileManagerWindow::on_launch_finished(source, result, status);
    }
};

} // namespace infiltrator::files

using namespace infiltrator::files;

namespace {

void require(bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void await(const std::function<bool()> &ready)
{
    const gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!ready() && g_get_monotonic_time() < deadline) {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        g_usleep(1000);
    }
    require(ready(), "Timed out waiting for GTK state");
}

std::string uri(const std::filesystem::path &path)
{
    GFile *file = g_file_new_for_path(path.c_str());
    char *text = g_file_get_uri(file);
    const std::string result(text);
    g_free(text);
    g_object_unref(file);
    return result;
}

int reverse_names(gconstpointer left, gconstpointer right, gpointer)
{
    return -g_strcmp0(g_file_info_get_name(G_FILE_INFO(const_cast<gpointer>(left))),
                     g_file_info_get_name(G_FILE_INFO(const_cast<gpointer>(right))));
}

} // namespace

int main()
{
    char *temporary = g_dir_make_tmp("files-window-state-XXXXXX", nullptr);
    require(temporary != nullptr, "Cannot create test profile");
    const std::filesystem::path root(temporary);
    g_free(temporary);
    const auto config = root / "fresh" / "profile";
    g_setenv("XDG_CONFIG_HOME", config.c_str(), TRUE);
    g_setenv("GTK_A11Y", "none", TRUE);
    g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
    gtk_init();
    GtkApplication *application = gtk_application_new("org.infiltrator.Files.StateTest", G_APPLICATION_NON_UNIQUE);
    require(g_application_register(G_APPLICATION(application), nullptr, nullptr), "Cannot register test application");

    try {
        const auto outer = root / "disk";
        const auto inner = outer / "nested";
        const auto folder = inner / "folder";
        std::filesystem::create_directories(folder);
        std::ofstream(folder / "alpha.txt") << "a";
        std::ofstream(folder / "zeta.txt") << "zz";
        const MountedPlace outer_mount{"Outer", uri(outer), uri(outer), "outer", true};
        const MountedPlace inner_mount{"Inner", uri(inner), uri(inner), "inner", true};
        const MountedPlace alternate{"Alias", uri(folder), uri(inner), "inner", true};

        auto *window = new FileManagerWindow(application);
        FileManagerWindowTestAccess::mounts(*window, {outer_mount, inner_mount, alternate});
        require(FileManagerWindowTestAccess::row_count(*window) == 3, "Mount targets must own distinct rows");
        GtkWidget *inner_row = FileManagerWindowTestAccess::row(*window, inner_mount.uri);
        GtkWidget *alias_row = FileManagerWindowTestAccess::row(*window, alternate.uri);
        require(inner_row != alias_row, "One mount with two targets must not alias a row");
        window->present();
        gtk_widget_grab_focus(inner_row);
        const int previous_index = gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(inner_row));
        MountedPlace renamed = inner_mount;
        renamed.name = "Aardvark";
        FileManagerWindowTestAccess::mounts(*window, {outer_mount, renamed, alternate});
        require(FileManagerWindowTestAccess::row(*window, inner_mount.uri) == inner_row, "Rename must preserve row identity");
        require(gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(inner_row)) < previous_index, "Renamed rows must reorder");
        require(gtk_window_get_focus(window->native_window()) == inner_row, "Reorder must retain keyboard focus");
        require(g_strcmp0(static_cast<const char *>(g_object_get_data(G_OBJECT(alias_row), "ifm-target")), alternate.uri.c_str()) == 0,
                "Alias row must keep its own navigation target");

        FileManagerWindowTestAccess::navigate(*window, folder);
        await([&] { return !gtk_directory_list_is_loading(FileManagerWindowTestAccess::directory(*window)); });
        require(g_list_model_get_n_items(G_LIST_MODEL(FileManagerWindowTestAccess::directory(*window))) == 2,
                "Directory fixture must load");
        GtkMultiSelection *selection = FileManagerWindowTestAccess::selection(*window);
        auto *presentation = GTK_SORT_LIST_MODEL(gtk_multi_selection_get_model(selection));
        GtkSorter *sorter = GTK_SORTER(gtk_custom_sorter_new(reverse_names, nullptr, nullptr));
        gtk_sort_list_model_set_sorter(presentation, sorter);
        g_object_unref(sorter);
        await([&] { return gtk_sort_list_model_get_pending(presentation) == 0; });
        gtk_selection_model_select_item(GTK_SELECTION_MODEL(selection), 0, TRUE);
        auto *primary = FileManagerWindowTestAccess::primary(*window);
        auto *selected = G_FILE_INFO(gtk_single_selection_get_selected_item(primary));
        require(selected != nullptr && g_strcmp0(g_file_info_get_name(selected), "zeta.txt") == 0,
                "Sorted selection must resolve to the same raw-directory item");

        FileManagerWindowTestAccess::mounts(*window, {outer_mount});
        require(!FileManagerWindowTestAccess::available(*window), "Outer mount must not mask inner removal");
        require(gtk_directory_list_get_file(FileManagerWindowTestAccess::directory(*window)) == nullptr,
                "Unavailable directory must clear the operation source");
        require(g_str_has_prefix(gtk_label_get_text(FileManagerWindowTestAccess::status(*window)), "Location unavailable."),
                "Unavailability must have neutral visible status");
        MountedPlace replacement = inner_mount;
        replacement.mount_uuid = "different-device";
        FileManagerWindowTestAccess::mounts(*window, {outer_mount, replacement});
        require(!FileManagerWindowTestAccess::available(*window), "Same path with another UUID must not restore");
        FileManagerWindowTestAccess::mounts(*window, {outer_mount, inner_mount});
        require(FileManagerWindowTestAccess::available(*window), "Matching source UUID must restore");

        MountedPlace weak = inner_mount;
        weak.mount_uuid.clear();
        FileManagerWindowTestAccess::mounts(*window, {weak});
        FileManagerWindowTestAccess::navigate(*window, folder);
        FileManagerWindowTestAccess::mounts(*window, {});
        FileManagerWindowTestAccess::mounts(*window, {weak});
        require(!FileManagerWindowTestAccess::available(*window), "Unproved reappearance must require retry");
        FileManagerWindowTestAccess::navigate(*window, folder);
        require(FileManagerWindowTestAccess::available(*window), "Explicit navigation must retry weak identities");

        require(FileManagerWindowTestAccess::watch_name(*window) == "fresh", "Missing config parents must watch the nearest ancestor");
        std::filesystem::create_directories(config / "infiltrator");
        await([&] { return FileManagerWindowTestAccess::watching_policy(*window); });

        // Keep external model references after the window is destroyed. No
        // model callback may retain the deleted C++ window as user_data.
        g_object_ref(selection);
        g_object_ref(primary);
        GtkDirectoryList *directory = FileManagerWindowTestAccess::directory(*window);
        g_object_ref(directory);
        bool finalized = false;
        g_object_weak_ref(G_OBJECT(window->native_window()), [](gpointer data, GObject *) {
            *static_cast<bool *>(data) = true;
        }, &finalized);
        struct LaunchCompletion { GtkLabel *status; bool complete; } completion{
            GTK_LABEL(g_object_ref(FileManagerWindowTestAccess::status(*window))), false};
        g_app_info_launch_default_for_uri_async("files-test-no-handler://missing", nullptr, nullptr,
            [](GObject *source, GAsyncResult *result, gpointer data) {
                auto *pending = static_cast<LaunchCompletion *>(data);
                FileManagerWindowTestAccess::finish_launch(source, result, pending->status);
                pending->complete = true;
            }, &completion);
        gtk_window_destroy(window->native_window());
        await([&] { return finalized && completion.complete; });
        gtk_single_selection_set_selected(primary, 0);
        gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(selection));
        GFile *reload = g_file_new_for_path(folder.c_str());
        gtk_directory_list_set_file(directory, reload);
        g_object_unref(reload);
        await([&] { return !gtk_directory_list_is_loading(directory); });
        g_object_unref(primary);
        g_object_unref(selection);
        g_object_unref(directory);
        for (unsigned iteration = 0; iteration < 8; ++iteration) {
            auto *repeated = new FileManagerWindow(application);
            gtk_window_destroy(repeated->native_window());
            while (g_main_context_iteration(nullptr, FALSE)) {}
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    g_object_unref(application);
    std::filesystem::remove_all(root);
    return 0;
}
