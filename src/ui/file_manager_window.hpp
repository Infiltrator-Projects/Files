// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../core/location.hpp"
#include "../core/mounted_places.hpp"
#include "batch_item_operation_controller.hpp"
#include "create_folder_controller.hpp"
#include "destructive_operation_controller.hpp"
#include "item_operation_controller.hpp"
#include "journal_recovery_controller.hpp"

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
    void install_operation_surface()
    {
        if (operation_surface_installed_) {
            return;
        }
        new CreateFolderController(GTK_WINDOW(window_), directory_list_, primary_selection_, status_label_);
        new ItemOperationController(GTK_WINDOW(window_), directory_list_, primary_selection_, status_label_);
        new DestructiveOperationController(GTK_WINDOW(window_), directory_list_, primary_selection_, status_label_);
        new BatchItemOperationController(GTK_WINDOW(window_), directory_list_, selection_, status_label_);
        new JournalRecoveryController(GTK_WINDOW(window_), status_label_);
        operation_surface_installed_ = true;
    }
    [[nodiscard]] GtkWindow *native_window() const noexcept { return GTK_WINDOW(window_); }

private:
    friend struct FileManagerWindowTestAccess;
    enum class ViewMode {
        List,
        Icons,
        Compact,
    };

    struct MountedPlaceRow {
        MountedPlace place;
        GtkWidget *row{nullptr};
    };

    static void on_window_finalized(gpointer user_data, GObject *where_object_was);
    static void on_back_clicked(GtkButton *button, gpointer user_data);
    static void on_forward_clicked(GtkButton *button, gpointer user_data);
    static void on_up_clicked(GtkButton *button, gpointer user_data);
    static void on_location_activate(GtkEntry *entry, gpointer user_data);
    static void on_sidebar_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data);
    static void on_list_activate(GtkColumnView *view, guint position, gpointer user_data);
    static void on_grid_activate(GtkGridView *view, guint position, gpointer user_data);
    static void on_view_mode_toggled(GtkToggleButton *button, gpointer user_data);
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
    GtkWidget *back_button_{nullptr};
    GtkWidget *forward_button_{nullptr};
    GtkWidget *up_button_{nullptr};
    GtkWidget *location_entry_{nullptr};
    GtkWidget *sidebar_{nullptr};
    GtkWidget *content_stack_{nullptr};
    GtkWidget *list_view_button_{nullptr};
    GtkWidget *icon_view_button_{nullptr};
    GtkWidget *compact_view_button_{nullptr};
    GtkWidget *status_label_{nullptr};
    GtkWidget *spinner_{nullptr};
    std::unique_ptr<MountedPlacesMonitor> mounted_places_monitor_;
    std::vector<MountedPlace> mounted_places_;
    std::vector<MountedPlaceRow> mounted_place_rows_;
    std::optional<MountedPlace> unavailable_mounted_place_;
    std::size_t static_sidebar_row_count_{0};
    GtkDirectoryList *directory_list_{nullptr};
    GtkMultiSelection *selection_{nullptr};
    GtkSingleSelection *primary_selection_{nullptr};
    GFile *current_location_{nullptr};
    GFileMonitor *temporal_policy_monitor_{nullptr};
    std::unordered_set<GtkWidget *> modified_cells_;
    std::vector<Location> history_;
    std::size_t history_index_{0};
    ViewMode view_mode_{ViewMode::List};
    bool operation_surface_installed_{false};
    bool selection_syncing_{false};
    bool current_location_available_{true};
    bool temporal_policy_monitoring_parent_{false};
    std::string temporal_policy_watch_name_;
};

} // namespace infiltrator::files
