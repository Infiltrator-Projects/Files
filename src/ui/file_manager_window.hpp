// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../core/location.hpp"
#include "../core/mounted_places.hpp"
#include "thumbnail_cache.hpp"

#include <gtk/gtk.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace infiltrator::files {

class FileManagerWindow final {
public:
    explicit FileManagerWindow(GtkApplication *application);
    ~FileManagerWindow();

    FileManagerWindow(const FileManagerWindow &) = delete;
    FileManagerWindow &operator=(const FileManagerWindow &) = delete;

    void present();
    void install_operation_surface();
    [[nodiscard]] bool open_primary_directory_in_new_tab();
    [[nodiscard]] bool open_primary_directory_in_new_window();
    void open_new_window(bool selected_folder = false);
    void duplicate_active_tab();
    void reopen_closed_tab();
    void move_active_tab(int delta);
    void toggle_show_hidden();
    void toggle_folders_first();
    void toggle_sidebar();
    [[nodiscard]] bool show_hidden() const noexcept { return show_hidden_; }
    [[nodiscard]] bool folders_first() const noexcept { return folders_first_; }
    [[nodiscard]] bool sidebar_visible() const noexcept { return sidebar_visible_; }
    [[nodiscard]] bool begin_inline_rename();
    [[nodiscard]] GtkWindow *native_window() const noexcept { return GTK_WINDOW(window_); }

private:
    friend struct FileManagerWindowTestAccess;
    enum class ViewMode {
        List,
        Icons,
        Compact,
    };

    struct TabState {
        std::vector<Location> history;
        std::size_t index{0};
        std::string uri;
        std::vector<std::string> selected_names;
        ViewMode view{ViewMode::List};
        GtkWidget *button{nullptr};
        GtkWidget *label{nullptr};
    };

    struct BookmarkRow {
        std::string uri;
        GtkWidget *row{nullptr};
    };

    struct MountedPlaceRow {
        MountedPlace place;
        GtkWidget *row{nullptr};
    };

    static void on_window_finalized(gpointer user_data, GObject *where_object_was);
    static void on_tab_clicked(GtkButton *button, gpointer user_data);
    static void on_tab_close_clicked(GtkButton *button, gpointer user_data);
    static void on_tab_add_clicked(GtkButton *button, gpointer user_data);
    static gboolean on_tab_key_pressed(GtkEventControllerKey *controller, guint keyval,
                                       guint keycode, GdkModifierType state, gpointer user_data);
    static gboolean filter_visible(gpointer item, gpointer user_data);
    static void on_bookmark_toggle(GtkButton *button, gpointer user_data);
    static void on_bookmark_action(GtkButton *button, gpointer user_data);
    static void on_back_clicked(GtkButton *button, gpointer user_data);
    static void on_forward_clicked(GtkButton *button, gpointer user_data);
    static void on_up_clicked(GtkButton *button, gpointer user_data);
    static void on_location_activate(GtkEntry *entry, gpointer user_data);
    static void on_sidebar_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data);
    static void on_list_activate(GtkColumnView *view, guint position, gpointer user_data);
    static void on_grid_activate(GtkGridView *view, guint position, gpointer user_data);
    static void on_view_mode_toggled(GtkToggleButton *button, gpointer user_data);
    static void on_inline_entry_activate(GtkEntry *entry, gpointer user_data);
    static gboolean on_inline_key(GtkEventControllerKey *controller, guint keyval,
                                   guint keycode, GdkModifierType state, gpointer user_data);
    static void on_inline_focus_leave(GtkEventControllerFocus *focus, gpointer user_data);
    static void on_icon_factory_unbind(GtkSignalListItemFactory *factory, GtkListItem *item,
                                       gpointer user_data);
    static void on_factory_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data);
    static void on_factory_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user_data);
    static void on_factory_unbind(GtkSignalListItemFactory *factory,
                                  GtkListItem *item,
                                  gpointer user_data);
    static void on_icon_factory_setup(GtkSignalListItemFactory *factory,
                                      GtkListItem *item,
                                      gpointer user_data);
    static void on_icon_factory_bind(GtkSignalListItemFactory *factory,
                                     GtkListItem *item,
                                     gpointer user_data);
    static void on_compact_factory_setup(GtkSignalListItemFactory *factory,
                                         GtkListItem *item,
                                         gpointer user_data);
    static void on_loading_changed(GObject *object, GParamSpec *pspec, gpointer user_data);
    static void on_launch_finished(GObject *source, GAsyncResult *result, gpointer user_data);
    static void on_multi_selection_changed(GtkSelectionModel *model,
                                           guint position,
                                           guint n_items,
                                           gpointer user_data);
    static void on_primary_selection_changed(GObject *object,
                                             GParamSpec *pspec,
                                             gpointer user_data);
    static void on_temporal_policy_changed(GFileMonitor *monitor,
                                           GFile *file,
                                           GFile *other_file,
                                           GFileMonitorEvent event_type,
                                           gpointer user_data);
    static void on_modified_cell_finalized(gpointer user_data,
                                           GObject *where_object_was);

    void build_ui(GtkApplication *application);
    GtkWidget *make_inline_editor(GtkWidget *cell, GtkWidget *label);
    void cancel_inline_editor(GtkWidget *cell);
    static GtkWidget *find_editor_for_name(GtkWidget *root, const char *name);
    void add_tab(GFile *file);
    void load_preferences();
    void save_preferences() const;
    void apply_preferences();
    void notify_sort_changed();
    void activate_tab(std::size_t index);
    void close_tab(std::size_t index);
    void remember_tab();
    void update_tab_buttons();
    void restore_tab_selection();
    void load_bookmarks();
    void save_bookmarks() const;
    void append_bookmark(const std::string &uri);
    void remove_bookmark(std::size_t index);
    void reorder_bookmarks();
    void update_bookmark_button();
    void apply_theme();
    void add_sidebar_location(const char *title, const char *icon_name, const char *target);
    void refresh_mounted_places();
    void apply_mounted_places(const std::vector<MountedPlace> &places);
    void reconcile_mounted_place_rows(const std::vector<MountedPlace> &places);
    void mark_current_location_unavailable(const MountedPlace &source);
    void restore_current_location_if_proven(const MountedPlace *source);
    void navigate_to(GFile *file, bool record_history);
    void navigate_history(std::ptrdiff_t delta);
    void activate_position(guint position);
    void set_view_mode(ViewMode mode);
    void update_navigation_state();
    void update_status();
    void sync_primary_from_multi();
    void sync_multi_from_primary();
    void arm_temporal_policy_monitor();
    void refresh_modified_cells();
    [[nodiscard]] GFile *file_from_location_text(const char *text) const;

    GtkWidget *window_{nullptr};
    GtkWidget *tab_strip_{nullptr};
    GtkWidget *bookmark_button_{nullptr};
    std::vector<TabState> tabs_;
    std::vector<TabState> recently_closed_tabs_;
    std::size_t active_tab_{0};
    std::vector<std::string> pending_tab_selection_;
    std::vector<BookmarkRow> bookmarks_;
    GtkWidget *back_button_{nullptr};
    GtkWidget *forward_button_{nullptr};
    GtkWidget *up_button_{nullptr};
    GtkWidget *location_entry_{nullptr};
    GtkWidget *sidebar_{nullptr};
    GtkWidget *sidebar_scroll_{nullptr};
    GtkWidget *header_view_switcher_{nullptr};
    GtkColumnView *detail_view_{nullptr};
    GtkWidget *content_stack_{nullptr};
    GtkWidget *list_view_button_{nullptr};
    GtkWidget *icon_view_button_{nullptr};
    GtkWidget *compact_view_button_{nullptr};
    GtkWidget *status_label_{nullptr};
    GtkWidget *spinner_{nullptr};
    std::unique_ptr<MountedPlacesMonitor> mounted_places_monitor_;
    std::unique_ptr<ThumbnailCache> thumbnails_;
    std::vector<MountedPlace> mounted_places_;
    std::vector<MountedPlaceRow> mounted_place_rows_;
    std::optional<MountedPlace> unavailable_mounted_place_;
    std::size_t static_sidebar_row_count_{0};
    GtkDirectoryList *directory_list_{nullptr};
    GtkMultiSelection *selection_{nullptr};
    GtkCustomFilter *visibility_filter_{nullptr};
    GtkSingleSelection *primary_selection_{nullptr};
    GFile *current_location_{nullptr};
    GFileMonitor *temporal_policy_monitor_{nullptr};
    std::unordered_set<GtkWidget *> modified_cells_;
    std::vector<Location> history_;
    std::size_t history_index_{0};
    ViewMode view_mode_{ViewMode::List};
    bool show_hidden_{false};
    bool folders_first_{true};
    bool sidebar_visible_{true};
    bool preferences_ready_{false};
    bool operation_surface_installed_{false};
    bool selection_syncing_{false};
    bool current_location_available_{true};
    bool temporal_policy_monitoring_parent_{false};
    std::string temporal_policy_watch_name_;
};

} // namespace infiltrator::files
