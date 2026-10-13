// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/ui/file_manager_window.hpp"
#include "../src/ui/theme_controller.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
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
    static void rearm_temporal_policy(FileManagerWindow &window)
    {
        window.arm_temporal_policy_monitor();
    }
    static std::string watch_name(const FileManagerWindow &window)
    {
        return window.temporal_policy_watch_name_;
    }
    static GtkLabel *status(FileManagerWindow &window) { return GTK_LABEL(window.status_label_); }
    static std::array<GtkWidget *, 3> view_buttons(FileManagerWindow &window)
    {
        return {window.list_view_button_, window.icon_view_button_,
                window.compact_view_button_};
    }
    static GtkWidget *sidebar(FileManagerWindow &window) { return window.sidebar_; }
    static GtkWidget *tab_strip(FileManagerWindow &window) { return window.tab_strip_; }
    static const std::optional<MountedPlace> &unavailable_source(
        const FileManagerWindow &window)
    {
        return window.unavailable_mounted_place_;
    }
    static FileManagerWindow *owner(GtkWindow *native)
    {
        return static_cast<FileManagerWindow *>(
            g_object_get_data(G_OBJECT(native), "ifm-window-owner"));
    }
    static GFile *current(FileManagerWindow &window) { return window.current_location_; }
    static std::size_t tab_count(const FileManagerWindow &window)
    {
        return window.browse_session_.has_value()
                   ? window.browse_session_->tabs().size()
                   : 0U;
    }
    static std::size_t closed_tab_count(const FileManagerWindow &window)
    {
        return window.browse_session_.has_value()
                   ? window.browse_session_->closed_count()
                   : 0U;
    }
    static std::optional<std::size_t> active_tab(const FileManagerWindow &window)
    {
        return window.browse_session_.has_value()
                   ? window.browse_session_->active_index()
                   : std::nullopt;
    }
    static void activate_tab(FileManagerWindow &window, const std::size_t index)
    {
        window.activate_tab(index);
    }
    static GtkAdjustment *scroll(FileManagerWindow &window)
    {
        return window.active_scroll_adjustment();
    }
    static const BrowseTabState *tab(const FileManagerWindow &window,
                                     const std::size_t index)
    {
        return window.browse_session_.has_value()
                   ? window.browse_session_->tab(index)
                   : nullptr;
    }
    static std::uint64_t tab_load_generation(const FileManagerWindow &window)
    {
        return window.tab_load_generation_;
    }
    static bool restore_tab_context(FileManagerWindow &window,
                                    const std::uint64_t generation)
    {
        return window.restore_tab_context_if_current(generation);
    }
    static guint queue_tab_context_restore(FileManagerWindow &window)
    {
        window.pending_tab_context_restore_ = true;
        window.schedule_tab_context_restore();
        return window.tab_context_restore_source_id_;
    }
    static void finish_launch(GObject *source, GAsyncResult *result, GtkLabel *status)
    {
        FileManagerWindow::on_launch_finished(source, result, status);
    }
};

struct ThemeControllerTestAccess {
    static ThemeController *for_window(GtkWindow *window)
    {
        for (ThemeController *controller : ThemeController::controllers_) {
            if (controller->window_ == window) {
                return controller;
            }
        }
        return nullptr;
    }
    static GtkWidget *mode_button(ThemeController &controller,
                                  const InfiltratrThemeMode mode)
    {
        return controller.mode_buttons_[static_cast<std::size_t>(mode)];
    }
    static InfiltratrThemeMode mode(const ThemeController &controller)
    {
        return controller.mode_;
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
        // Use real scrollable content so context restoration is exercised
        // against GTK's layout-owned adjustment bounds, not synthetic bounds
        // that a later allocation is allowed to replace.
        for (unsigned index = 0; index < 96; ++index) {
            std::ofstream(folder / ("entry-" + std::to_string(index) + ".txt"))
                << index;
        }
        const MountedPlace outer_mount{"Outer", uri(outer), uri(outer), "outer", true};
        const MountedPlace inner_mount{"Inner", uri(inner), uri(inner), "inner", true};
        const MountedPlace alternate{"Alias", uri(folder), uri(inner), "inner", true};

        auto *window = new FileManagerWindow(application);
        new ThemeController(window->native_window());

        for (GtkWidget *button : FileManagerWindowTestAccess::view_buttons(*window)) {
            require(GTK_IS_IMAGE(gtk_button_get_child(GTK_BUTTON(button))),
                    "View switcher controls must use symbolic images");
            require(gtk_widget_get_tooltip_text(button) != nullptr,
                    "Symbolic view controls must retain discoverable tooltips");
        }
        GtkWidget *paned = FileManagerWindowTestAccess::sidebar(*window);
        while (paned != nullptr && !GTK_IS_PANED(paned)) {
            paned = gtk_widget_get_parent(paned);
        }
        require(paned != nullptr &&
                    gtk_paned_get_shrink_start_child(GTK_PANED(paned)),
                "Sidebar must yield space in narrow layouts");


        struct SessionShortcut {
            const char *action;
            const char *accelerator;
        };
        const SessionShortcut session_shortcuts[] = {
            {"win.new-window", "<Control>n"},
            {"win.new-tab", "<Control>t"},
            {"win.close-tab", "<Control>w"},
            {"win.reopen-closed-tab", "<Control><Shift>t"},
            {"win.duplicate-tab", "<Control><Shift>d"},
            {"win.move-tab-left", "<Control><Shift>Page_Up"},
            {"win.move-tab-right", "<Control><Shift>Page_Down"},
            {"win.move-tab-new-window", "<Control><Shift>m"},
        };
        std::set<std::pair<guint, GdkModifierType>> distinct_shortcuts;
        guint new_folder_key = 0;
        GdkModifierType new_folder_modifiers = static_cast<GdkModifierType>(0);
        require(gtk_accelerator_parse("<Control><Shift>n", &new_folder_key,
                                      &new_folder_modifiers),
                "Reserved New Folder accelerator must parse");
        for (const auto &shortcut : session_shortcuts) {
            char **configured =
                gtk_application_get_accels_for_action(application, shortcut.action);
            require(configured != nullptr && configured[0] != nullptr &&
                        configured[1] == nullptr,
                    "Each session action must expose exactly one accelerator");
            guint configured_key = 0;
            guint expected_key = 0;
            GdkModifierType configured_modifiers = static_cast<GdkModifierType>(0);
            GdkModifierType expected_modifiers = static_cast<GdkModifierType>(0);
            require(gtk_accelerator_parse(configured[0], &configured_key,
                                          &configured_modifiers) &&
                        gtk_accelerator_parse(shortcut.accelerator, &expected_key,
                                              &expected_modifiers),
                    "Session accelerators must parse");
            require(configured_key == expected_key &&
                        configured_modifiers == expected_modifiers,
                    "Session action accelerator must match the documented map");
            require(distinct_shortcuts
                        .emplace(configured_key, configured_modifiers)
                        .second,
                    "Session accelerators must not collide");
            require(configured_key != new_folder_key ||
                        configured_modifiers != new_folder_modifiers,
                    "Session accelerators must preserve Ctrl+Shift+N for New Folder");
            g_strfreev(configured);
        }

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
        require(g_list_model_get_n_items(G_LIST_MODEL(FileManagerWindowTestAccess::directory(*window))) == 98,
                "Directory fixture must load");

        const guint window_count =
            g_list_length(gtk_application_get_windows(application));
        g_action_group_activate_action(
            G_ACTION_GROUP(window->native_window()), "new-window", nullptr);
        await([&] {
            return g_list_length(gtk_application_get_windows(application)) ==
                   window_count + 1;
        });
        FileManagerWindow *spawned = nullptr;
        for (GList *node = gtk_application_get_windows(application);
             node != nullptr; node = node->next) {
            if (node->data != window->native_window()) {
                spawned = FileManagerWindowTestAccess::owner(
                    GTK_WINDOW(node->data));
                if (spawned != nullptr) {
                    break;
                }
            }
        }
        require(spawned != nullptr,
                "New-window action must create an owned Files window");
        ThemeController *source_theme =
            ThemeControllerTestAccess::for_window(window->native_window());
        ThemeController *spawned_theme =
            ThemeControllerTestAccess::for_window(spawned->native_window());
        require(source_theme != nullptr && spawned_theme != nullptr,
                "Every live window must participate in shared appearance state");
        gtk_check_button_set_active(
            GTK_CHECK_BUTTON(ThemeControllerTestAccess::mode_button(
                *source_theme, INFILTRATR_THEME_NIGHT)), TRUE);
        require(ThemeControllerTestAccess::mode(*source_theme) ==
                    INFILTRATR_THEME_NIGHT &&
                    ThemeControllerTestAccess::mode(*spawned_theme) ==
                    INFILTRATR_THEME_NIGHT &&
                    gtk_check_button_get_active(GTK_CHECK_BUTTON(
                        ThemeControllerTestAccess::mode_button(
                            *spawned_theme, INFILTRATR_THEME_NIGHT))),
                "Changing appearance in one window must synchronize every live window");
        require(g_file_equal(FileManagerWindowTestAccess::current(*window),
                             FileManagerWindowTestAccess::current(*spawned)),
                "New window must inherit the intended current location");
        GFile *source_before =
            G_FILE(g_object_ref(FileManagerWindowTestAccess::current(*window)));
        FileManagerWindowTestAccess::navigate(*spawned, root);
        require(g_file_equal(FileManagerWindowTestAccess::current(*window),
                             source_before),
                "Navigating the new window must not change the source window");
        require(!g_file_equal(FileManagerWindowTestAccess::current(*window),
                              FileManagerWindowTestAccess::current(*spawned)),
                "Independent windows must retain distinct current locations");
        g_object_unref(source_before);
        gtk_window_destroy(spawned->native_window());
        await([&] {
            return g_list_length(gtk_application_get_windows(application)) ==
                   window_count;
        });
        // The appearance assertion intentionally persists a preference. Reset
        // that independent side effect so the temporal-policy test below still
        // begins from its specified missing-parent profile state.
        std::filesystem::remove_all(config);
        FileManagerWindowTestAccess::rearm_temporal_policy(*window);

        require(FileManagerWindowTestAccess::tab_count(*window) == 1U,
                "A Files window must start with one browse tab");
        g_action_group_activate_action(
            G_ACTION_GROUP(window->native_window()), "new-tab", nullptr);
        await([&] {
            return !gtk_directory_list_is_loading(
                FileManagerWindowTestAccess::directory(*window));
        });
        require(FileManagerWindowTestAccess::tab_count(*window) == 2U,
                "New-tab action must add and activate an independent tab");
        GtkWidget *tab_strip = FileManagerWindowTestAccess::tab_strip(*window);
        GtkWidget *tab_scroll = gtk_widget_get_parent(tab_strip);
        while (tab_scroll != nullptr && !GTK_IS_SCROLLED_WINDOW(tab_scroll)) {
            tab_scroll = gtk_widget_get_parent(tab_scroll);
        }
        require(tab_scroll != nullptr,
                "The tab strip must remain horizontally reachable at narrow widths");
        guint active_tab_controls = 0U;
        for (GtkWidget *tab_button = gtk_widget_get_first_child(tab_strip);
             tab_button != nullptr;
             tab_button = gtk_widget_get_next_sibling(tab_button)) {
            require(GTK_IS_TOGGLE_BUTTON(tab_button),
                    "Location tabs must expose an active toggle state");
            active_tab_controls += gtk_toggle_button_get_active(
                GTK_TOGGLE_BUTTON(tab_button)) ? 1U : 0U;
            GtkWidget *tab_label = gtk_button_get_child(GTK_BUTTON(tab_button));
            require(GTK_IS_LABEL(tab_label) &&
                        gtk_label_get_max_width_chars(GTK_LABEL(tab_label)) == 24 &&
                        gtk_label_get_ellipsize(GTK_LABEL(tab_label)) ==
                            PANGO_ELLIPSIZE_END,
                    "Tab labels must be bounded and ellipsized for narrow windows");
        }
        require(active_tab_controls == 1U,
                "Exactly one location tab must expose the active state");
        FileManagerWindowTestAccess::navigate(*window, root);
        await([&] {
            return !gtk_directory_list_is_loading(
                FileManagerWindowTestAccess::directory(*window));
        });
        FileManagerWindowTestAccess::activate_tab(*window, 0U);
        await([&] {
            return !gtk_directory_list_is_loading(
                FileManagerWindowTestAccess::directory(*window));
        });
        GFile *source_tab_file = g_file_new_for_path(folder.c_str());
        require(g_file_equal(FileManagerWindowTestAccess::current(*window),
                             source_tab_file),
                "Switching tabs must restore the source tab location");
        g_object_unref(source_tab_file);

        GtkMultiSelection *context_selection =
            FileManagerWindowTestAccess::selection(*window);
        GListModel *context_model =
            gtk_multi_selection_get_model(context_selection);
        guint alpha_position = GTK_INVALID_LIST_POSITION;
        for (guint index = 0; index < g_list_model_get_n_items(context_model);
             ++index) {
            GFileInfo *info =
                G_FILE_INFO(g_list_model_get_item(context_model, index));
            if (info != nullptr &&
                g_strcmp0(g_file_info_get_name(info), "alpha.txt") == 0) {
                alpha_position = index;
            }
            if (info != nullptr) {
                g_object_unref(info);
            }
        }
        require(alpha_position != GTK_INVALID_LIST_POSITION,
                "Selection fixture must expose alpha.txt");
        gtk_selection_model_select_item(
            GTK_SELECTION_MODEL(context_selection), alpha_position, TRUE);

        GtkAdjustment *context_scroll =
            FileManagerWindowTestAccess::scroll(*window);
        require(context_scroll != nullptr,
                "Active presentation must expose a vertical adjustment");
        await([&] {
            return gtk_adjustment_get_upper(context_scroll) -
                       gtk_adjustment_get_page_size(context_scroll) >=
                   37.0;
        });
        gtk_adjustment_set_value(context_scroll, 37.0);
        require(gtk_adjustment_get_value(context_scroll) == 37.0,
                "Scrollable fixture must accept the context position");

        const std::uint64_t superseded_generation =
            FileManagerWindowTestAccess::tab_load_generation(*window);
        FileManagerWindowTestAccess::activate_tab(*window, 1U);
        require(!FileManagerWindowTestAccess::restore_tab_context(
                    *window, superseded_generation),
                "A superseded tab load must not restore context into the active tab");
        await([&] {
            return !gtk_directory_list_is_loading(
                FileManagerWindowTestAccess::directory(*window));
        });
        const BrowseTabState *stored_context =
            FileManagerWindowTestAccess::tab(*window, 0U);
        require(stored_context != nullptr &&
                    stored_context->primary_selection == "alpha.txt" &&
                    stored_context->scroll_position == 37.0,
                "Switching away must persist selection and scroll context");

        FileManagerWindowTestAccess::activate_tab(*window, 0U);
        await([&] {
            return !gtk_directory_list_is_loading(
                FileManagerWindowTestAccess::directory(*window));
        });
        auto *restored_primary =
            FileManagerWindowTestAccess::primary(*window);
        auto *restored_info = G_FILE_INFO(
            gtk_single_selection_get_selected_item(restored_primary));
        require(restored_info != nullptr &&
                    g_strcmp0(g_file_info_get_name(restored_info),
                              "alpha.txt") == 0,
                "Switching back must restore the tab's primary selection");
        require(gtk_selection_model_is_selected(
                    GTK_SELECTION_MODEL(context_selection), alpha_position),
                "Switching back must restore the tab's multi-selection");
        await([&] {
            return FileManagerWindowTestAccess::scroll(*window) != nullptr &&
                   gtk_adjustment_get_value(
                       FileManagerWindowTestAccess::scroll(*window)) == 37.0;
        });
        require(gtk_adjustment_get_value(
                    FileManagerWindowTestAccess::scroll(*window)) == 37.0,
                "Switching back must restore the tab's scroll position");

        g_action_group_activate_action(
            G_ACTION_GROUP(window->native_window()), "close-tab", nullptr);
        await([&] {
            return !gtk_directory_list_is_loading(
                FileManagerWindowTestAccess::directory(*window));
        });
        require(FileManagerWindowTestAccess::tab_count(*window) == 1U &&
                    FileManagerWindowTestAccess::closed_tab_count(*window) == 1U,
                "Close-tab action must retain one bounded restorable snapshot");
        GFile *root_file = g_file_new_for_path(root.c_str());
        require(g_file_equal(FileManagerWindowTestAccess::current(*window),
                             root_file),
                "Closing the active tab must activate the surviving tab");
        g_object_unref(root_file);

        g_action_group_activate_action(
            G_ACTION_GROUP(window->native_window()),
            "reopen-closed-tab", nullptr);
        await([&] {
            return !gtk_directory_list_is_loading(
                FileManagerWindowTestAccess::directory(*window));
        });
        require(FileManagerWindowTestAccess::tab_count(*window) == 2U &&
                    FileManagerWindowTestAccess::closed_tab_count(*window) == 0U,
                "Reopen action must restore the most recently closed tab");
        GFile *folder_file = g_file_new_for_path(folder.c_str());
        require(g_file_equal(FileManagerWindowTestAccess::current(*window),
                             folder_file),
                "Reopened tab must restore its browsing location");
        g_object_unref(folder_file);

        g_action_group_activate_action(
            G_ACTION_GROUP(window->native_window()), "duplicate-tab", nullptr);
        await([&] {
            return !gtk_directory_list_is_loading(
                FileManagerWindowTestAccess::directory(*window));
        });
        require(FileManagerWindowTestAccess::tab_count(*window) == 3U &&
                    FileManagerWindowTestAccess::active_tab(*window) == 2U,
                "Duplicate action must insert and activate an adjacent tab");
        FileManagerWindowTestAccess::navigate(*window, root);
        await([&] {
            return !gtk_directory_list_is_loading(
                FileManagerWindowTestAccess::directory(*window));
        });
        FileManagerWindowTestAccess::activate_tab(*window, 1U);
        await([&] {
            return !gtk_directory_list_is_loading(
                FileManagerWindowTestAccess::directory(*window));
        });
        GFile *original_folder_file = g_file_new_for_path(folder.c_str());
        require(g_file_equal(FileManagerWindowTestAccess::current(*window),
                             original_folder_file),
                "Navigating a duplicate must not mutate the source tab");
        g_object_unref(original_folder_file);

        FileManagerWindowTestAccess::activate_tab(*window, 2U);
        await([&] {
            return !gtk_directory_list_is_loading(
                FileManagerWindowTestAccess::directory(*window));
        });
        g_action_group_activate_action(
            G_ACTION_GROUP(window->native_window()), "move-tab-left", nullptr);
        require(FileManagerWindowTestAccess::active_tab(*window) == 1U,
                "Move-left action must preserve active-tab identity");
        g_action_group_activate_action(
            G_ACTION_GROUP(window->native_window()), "move-tab-right", nullptr);
        require(FileManagerWindowTestAccess::active_tab(*window) == 2U,
                "Move-right action must restore the active tab position");
        GFile *reordered_root_file = g_file_new_for_path(root.c_str());
        require(g_file_equal(FileManagerWindowTestAccess::current(*window),
                             reordered_root_file),
                "Reordering must preserve the active tab browsing context");
        g_object_unref(reordered_root_file);

        // The reorder assertions intentionally leave the duplicate at the
        // fixture root. Restore the original folder tab before continuing
        // with the pre-existing sorted-selection and mount-identity checks.
        FileManagerWindowTestAccess::activate_tab(*window, 1U);
        await([&] {
            return !gtk_directory_list_is_loading(
                FileManagerWindowTestAccess::directory(*window));
        });
        GFile *selection_folder_file = g_file_new_for_path(folder.c_str());
        require(g_file_equal(FileManagerWindowTestAccess::current(*window),
                             selection_folder_file),
                "Selection checks must run against the original folder tab");
        g_object_unref(selection_folder_file);

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
        FileManagerWindowTestAccess::activate_tab(*window, 0U);
        await([&] {
            return !gtk_directory_list_is_loading(
                FileManagerWindowTestAccess::directory(*window));
        });
        FileManagerWindowTestAccess::activate_tab(*window, 1U);
        await([&] {
            return gtk_directory_list_get_file(
                       FileManagerWindowTestAccess::directory(*window)) == nullptr;
        });
        require(!FileManagerWindowTestAccess::available(*window) &&
                    FileManagerWindowTestAccess::unavailable_source(*window)
                        .has_value() &&
                    FileManagerWindowTestAccess::unavailable_source(*window)
                            ->mount_uuid == inner_mount.mount_uuid,
                "Tab switching must preserve unavailable source identity");
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

        const guint before_move_window_count =
            g_list_length(gtk_application_get_windows(application));
        g_action_group_activate_action(
            G_ACTION_GROUP(window->native_window()),
            "move-tab-new-window", nullptr);
        await([&] {
            return g_list_length(gtk_application_get_windows(application)) ==
                   before_move_window_count + 1U;
        });
        FileManagerWindow *moved_window = nullptr;
        for (GList *node = gtk_application_get_windows(application);
             node != nullptr; node = node->next) {
            if (node->data != window->native_window()) {
                moved_window = FileManagerWindowTestAccess::owner(
                    GTK_WINDOW(node->data));
                if (moved_window != nullptr) {
                    break;
                }
            }
        }
        require(moved_window != nullptr,
                "Move-to-new-window must create one owned destination window");
        require(FileManagerWindowTestAccess::tab_count(*window) == 2U &&
                    FileManagerWindowTestAccess::tab_count(*moved_window) == 1U,
                "Moving must transfer one tab to exactly one window");
        GFile *moved_folder_file = g_file_new_for_path(folder.c_str());
        require(g_file_equal(
                    FileManagerWindowTestAccess::current(*moved_window),
                    moved_folder_file),
                "Moved tab must preserve its browsing location");
        g_object_unref(moved_folder_file);
        GFile *surviving_root_file = g_file_new_for_path(root.c_str());
        require(g_file_equal(FileManagerWindowTestAccess::current(*window),
                             surviving_root_file),
                "Source window must activate its deterministic surviving tab");
        g_object_unref(surviving_root_file);
        gtk_window_destroy(moved_window->native_window());
        await([&] {
            return g_list_length(gtk_application_get_windows(application)) ==
                   before_move_window_count;
        });

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
        const guint pending_restore_source =
            FileManagerWindowTestAccess::queue_tab_context_restore(*window);
        require(pending_restore_source != 0U &&
                    g_main_context_find_source_by_id(nullptr,
                                                     pending_restore_source) != nullptr,
                "Destruction test must begin with a pending tab-context callback");
        gtk_window_destroy(window->native_window());
        await([&] { return finalized && completion.complete; });
        require(g_main_context_find_source_by_id(nullptr,
                                                 pending_restore_source) == nullptr,
                "Window destruction must cancel its pending tab-context callback");
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
