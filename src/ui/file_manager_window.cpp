// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_manager_window.hpp"
#include "detail_metadata.hpp"
#include "item_operation_controller.hpp"
#include "theme_controller.hpp"

#include <infiltratr/temporal_posix.h>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <string>

namespace infiltrator::files {

namespace {

constexpr const char *kDirectoryAttributes =
    "standard::name,standard::display-name,standard::type,standard::size,"
    "standard::content-type,standard::icon,standard::is-hidden,standard::is-symlink,"
    "time::modified,time::modified-usec,time::modified-nsec,"
    "time::created,unix::blocks,unix::mode,unix::uid,unix::gid";

constexpr const char *kLocationUnavailableStatus =
    "Location unavailable. Reopen the location to retry.";

constexpr int kDetailIconSize = 20;
constexpr int kDetailTypeWidth = 160;
constexpr int kDetailSizeWidth = 96;
constexpr int kDetailModifiedWidth = 190;

enum class DetailField {
    Name = 1,
    Type,
    Size,
    Modified,
};

struct DetailSortContext {
    std::shared_ptr<std::atomic_bool> folders_first;
    DetailField field{DetailField::Name};
};

GtkWidget *make_sidebar_content(const char *title, const char *icon_name)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 10);
    gtk_widget_set_margin_top(box, 5);
    gtk_widget_set_margin_bottom(box, 5);

    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 18);
    gtk_box_append(GTK_BOX(box), icon);

    GtkWidget *label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(box), label);
    return box;
}

GtkWidget *make_scroller(GtkWidget *child)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), child);
    gtk_widget_add_css_class(scroll, "ifm-browser-scroll");
    return scroll;
}

int compare_detail_items(gconstpointer left, gconstpointer right, gpointer user_data)
{
    auto *left_info = G_FILE_INFO(const_cast<gpointer>(left));
    auto *right_info = G_FILE_INFO(const_cast<gpointer>(right));
    const auto *context = static_cast<const DetailSortContext *>(user_data);
    if (left_info == nullptr || right_info == nullptr || context == nullptr) return 0;
    if (context->folders_first != nullptr &&
        context->folders_first->load(std::memory_order_relaxed)) {
        const bool left_directory = g_file_info_get_file_type(left_info) == G_FILE_TYPE_DIRECTORY;
        const bool right_directory = g_file_info_get_file_type(right_info) == G_FILE_TYPE_DIRECTORY;
        if (left_directory != right_directory) return left_directory ? -1 : 1;
    }
    switch (context->field) {
    case DetailField::Name:
        return detail_compare_name(left_info, right_info);
    case DetailField::Type:
        return detail_compare_type(left_info, right_info);
    case DetailField::Size:
        return detail_compare_size(left_info, right_info);
    case DetailField::Modified:
        return detail_compare_modified(left_info, right_info);
    }
    return 0;
}

bool same_directory_item(GFileInfo *left, GFileInfo *right)
{
    if (left == right) {
        return true;
    }
    if (left == nullptr || right == nullptr) {
        return false;
    }
    return g_strcmp0(g_file_info_get_name(left), g_file_info_get_name(right)) == 0;
}

guint find_item_position(GListModel *model, GFileInfo *needle)
{
    if (model == nullptr || needle == nullptr) {
        return GTK_INVALID_LIST_POSITION;
    }
    const guint count = g_list_model_get_n_items(model);
    for (guint index = 0U; index < count; ++index) {
        GFileInfo *candidate = G_FILE_INFO(g_list_model_get_item(model, index));
        const bool match = same_directory_item(candidate, needle);
        if (candidate != nullptr) {
            g_object_unref(candidate);
        }
        if (match) {
            return index;
        }
    }
    return GTK_INVALID_LIST_POSITION;
}

} // namespace

FileManagerWindow::FileManagerWindow(GtkApplication *application)
{
    build_ui(application);
    thumbnails_ = std::make_unique<ThumbnailCache>();
    g_object_set_data(G_OBJECT(window_), "ifm-window-controller", this);
    g_signal_connect_object(application, "window-removed", G_CALLBACK(+[](GtkApplication *app,
        GtkWindow *removed, gpointer owner) {
        if (removed != owner) return;
        for (GList *item = gtk_application_get_windows(app); item != nullptr; item = item->next) {
            if (item->data != removed &&
                g_object_get_data(G_OBJECT(item->data), "ifm-window-controller") != nullptr) return;
        }
        auto *self = static_cast<FileManagerWindow *>(
            g_object_get_data(G_OBJECT(removed), "ifm-window-controller"));
        if (self != nullptr) (void)self->save_session();
    }), window_, G_CONNECT_DEFAULT);
    arm_temporal_policy_monitor();
    load_bookmarks();
    load_recents();
    load_preferences();
    load_folder_views();
    apply_preferences();
    tabs_.push_back(TabState{});
    install_tab_drop();
    update_tab_buttons();

    g_object_weak_ref(G_OBJECT(window_), &FileManagerWindow::on_window_finalized, this);

    // The application opens at the filesystem root, not the login home folder.
    // Home remains one click away in Places and Alt+Home.
    GFile *root = g_file_new_for_path("/");
    navigate_to(root, true);
    g_object_unref(root);
}

FileManagerWindow::~FileManagerWindow()
{
    if (location_request_ != nullptr) g_cancellable_cancel(location_request_);
    g_clear_object(&location_request_);
    // Stop external callbacks before any UI/model references owned by this object are released.
    if (search_timeout_ != 0U) g_source_remove(search_timeout_);
    mounted_places_monitor_.reset();
    thumbnails_.reset();
    if (temporal_policy_monitor_ != nullptr) {
        g_signal_handlers_disconnect_by_data(temporal_policy_monitor_, this);
        g_object_unref(temporal_policy_monitor_);
        temporal_policy_monitor_ = nullptr;
    }
    for (GtkWidget *cell : modified_cells_) {
        g_object_weak_unref(
            G_OBJECT(cell), &FileManagerWindow::on_modified_cell_finalized, this);
    }
    modified_cells_.clear();

    // Model callbacks capture this object. Own each model until all callbacks
    // are disconnected, even if GTK disposes the views before the window.
    for (GObject *model : {G_OBJECT(directory_list_), G_OBJECT(selection_),
                          G_OBJECT(primary_selection_)}) {
        if (model != nullptr) {
            g_signal_handlers_disconnect_by_data(model, this);
        }
    }
    if (primary_selection_ != nullptr) {
        g_object_unref(primary_selection_);
        primary_selection_ = nullptr;
    }
    g_clear_object(&selection_);
    g_clear_object(&visibility_filter_);
    filter_state_ = nullptr; // Owned by GtkCustomFilter, which may outlive the window.
    g_clear_object(&directory_list_);
    if (current_location_ != nullptr) {
        g_object_unref(current_location_);
        current_location_ = nullptr;
    }
}

void FileManagerWindow::present()
{
    gtk_window_present(GTK_WINDOW(window_));
}

void FileManagerWindow::on_window_finalized(gpointer user_data, GObject *where_object_was)
{
    (void)where_object_was;
    delete static_cast<FileManagerWindow *>(user_data);
}

void FileManagerWindow::build_ui(GtkApplication *application)
{
    window_ = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(window_), "Files");
    gtk_window_set_default_size(GTK_WINDOW(window_), 1180, 780);

    GtkWidget *header = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(window_), header);

    GtkWidget *brand = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(brand, "ifm-brand");
    GtkWidget *brand_icon = gtk_image_new_from_icon_name("folder-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(brand_icon), 19);
    gtk_widget_add_css_class(brand_icon, "ifm-brand-mark");
    GtkWidget *brand_title = gtk_label_new("Files");
    gtk_widget_add_css_class(brand_title, "ifm-brand-title");
    gtk_box_append(GTK_BOX(brand), brand_icon);
    gtk_box_append(GTK_BOX(brand), brand_title);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), brand);

    GtkWidget *nav_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_widget_add_css_class(nav_box, "ifm-nav-actions");
    back_button_ = gtk_button_new_from_icon_name("go-previous-symbolic");
    forward_button_ = gtk_button_new_from_icon_name("go-next-symbolic");
    up_button_ = gtk_button_new_from_icon_name("go-up-symbolic");
    gtk_widget_set_tooltip_text(back_button_, "Back");
    gtk_widget_set_tooltip_text(forward_button_, "Forward");
    gtk_widget_set_tooltip_text(up_button_, "Up");
    gtk_box_append(GTK_BOX(nav_box), back_button_);
    gtk_box_append(GTK_BOX(nav_box), forward_button_);
    gtk_box_append(GTK_BOX(nav_box), up_button_);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), nav_box);

    location_entry_ = gtk_entry_new();
    gtk_widget_set_hexpand(location_entry_, TRUE);
    // Keep the path useful at normal widths without forcing the whole window to
    // remain wider than compact/split-screen layouts can provide.
    gtk_widget_set_size_request(location_entry_, 180, -1);
    gtk_entry_set_placeholder_text(GTK_ENTRY(location_entry_), "Location");
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), location_entry_);

    GtkWidget *view_switcher = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    header_view_switcher_ = view_switcher;
    // A native GTK linked group paints its own filled button backgrounds on
    // some Day themes. Files supplies one consistent Common-owned treatment.
    gtk_widget_add_css_class(view_switcher, "ifm-view-switcher");

    list_view_button_ = gtk_toggle_button_new();
    icon_view_button_ = gtk_toggle_button_new();
    compact_view_button_ = gtk_toggle_button_new();
    gtk_button_set_child(GTK_BUTTON(list_view_button_),
                         gtk_image_new_from_icon_name("view-list-symbolic"));
    gtk_button_set_child(GTK_BUTTON(icon_view_button_),
                         gtk_image_new_from_icon_name("view-grid-symbolic"));
    gtk_button_set_child(GTK_BUTTON(compact_view_button_),
                         gtk_image_new_from_icon_name("view-list-bullet-symbolic"));
    for (GtkWidget *button : {list_view_button_, icon_view_button_, compact_view_button_}) {
        gtk_widget_add_css_class(button, "ifm-view-button");
        gtk_widget_set_size_request(button, 32, 32);
    }
    gtk_widget_set_tooltip_text(list_view_button_, "Details (Ctrl+1)");
    gtk_widget_set_tooltip_text(icon_view_button_, "Icons (Ctrl+2)");
    gtk_widget_set_tooltip_text(compact_view_button_, "Compact (Ctrl+3)");

    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(icon_view_button_),
                                GTK_TOGGLE_BUTTON(list_view_button_));
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(compact_view_button_),
                                GTK_TOGGLE_BUTTON(list_view_button_));

    g_object_set_data(G_OBJECT(list_view_button_), "ifm-view-mode", GINT_TO_POINTER(1));
    g_object_set_data(G_OBJECT(icon_view_button_), "ifm-view-mode", GINT_TO_POINTER(2));
    g_object_set_data(G_OBJECT(compact_view_button_), "ifm-view-mode", GINT_TO_POINTER(3));

    g_signal_connect(list_view_button_, "toggled", G_CALLBACK(on_view_mode_toggled), this);
    g_signal_connect(icon_view_button_, "toggled", G_CALLBACK(on_view_mode_toggled), this);
    g_signal_connect(compact_view_button_, "toggled", G_CALLBACK(on_view_mode_toggled), this);

    gtk_box_append(GTK_BOX(view_switcher), list_view_button_);
    gtk_box_append(GTK_BOX(view_switcher), icon_view_button_);
    gtk_box_append(GTK_BOX(view_switcher), compact_view_button_);
    // View-mode controls belong to the quiet status strip, not the location bar.
    // The header is reserved for navigation, path and the single action menu.

    g_signal_connect(back_button_, "clicked", G_CALLBACK(on_back_clicked), this);
    g_signal_connect(forward_button_, "clicked", G_CALLBACK(on_forward_clicked), this);
    g_signal_connect(up_button_, "clicked", G_CALLBACK(on_up_clicked), this);
    g_signal_connect(location_entry_, "activate", G_CALLBACK(on_location_activate), this);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(window_), root);

    GtkWidget *tab_line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(tab_line, 0);
    gtk_widget_set_margin_end(tab_line, 0);
    gtk_widget_add_css_class(tab_line, "ifm-tab-line");
    gtk_box_append(GTK_BOX(root), tab_line);
    GtkWidget *tab_scroll = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(tab_scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tab_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    tab_strip_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_widget_set_margin_start(tab_strip_, 12);
    gtk_widget_set_margin_top(tab_strip_, 2);
    gtk_widget_set_margin_bottom(tab_strip_, 2);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tab_scroll), tab_strip_);
    gtk_box_append(GTK_BOX(tab_line), tab_scroll);
    search_entry_ = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search_entry_), "Find in folder");
    gtk_widget_set_size_request(search_entry_, 195, -1);
    gtk_widget_set_visible(search_entry_, FALSE);
    g_signal_connect(search_entry_, "search-changed", G_CALLBACK(on_search_changed), this);
    gtk_box_append(GTK_BOX(tab_line), search_entry_);
    search_button_ = gtk_button_new_from_icon_name("system-search-symbolic");
    gtk_widget_add_css_class(search_button_, "flat");
    gtk_widget_set_tooltip_text(search_button_, "Find in this folder (Ctrl+F)");
    g_signal_connect(search_button_, "clicked", G_CALLBACK(on_search_toggle), this);
    gtk_box_append(GTK_BOX(tab_line), search_button_);
    GtkWidget *add_tab_button = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_add_css_class(add_tab_button, "flat");
    gtk_widget_set_tooltip_text(add_tab_button, "New tab (Ctrl+T)");
    g_signal_connect(add_tab_button, "clicked", G_CALLBACK(on_tab_add_clicked), this);
    gtk_box_append(GTK_BOX(tab_line), add_tab_button);
    bookmark_button_ = gtk_button_new_from_icon_name("non-starred-symbolic");
    gtk_widget_add_css_class(bookmark_button_, "flat");
    gtk_widget_set_tooltip_text(bookmark_button_, "Pin or unpin current folder");
    g_signal_connect(bookmark_button_, "clicked", G_CALLBACK(on_bookmark_toggle), this);
    gtk_box_append(GTK_BOX(tab_line), bookmark_button_);
    gtk_widget_set_margin_end(bookmark_button_, 12);
    GtkEventController *tab_keys = gtk_event_controller_key_new();
    g_signal_connect(tab_keys, "key-pressed", G_CALLBACK(on_tab_key_pressed), this);
    gtk_widget_add_controller(window_, tab_keys);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_box_append(GTK_BOX(root), paned);

    sidebar_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(sidebar_), GTK_SELECTION_NONE);
    gtk_widget_set_size_request(sidebar_, 228, -1);
    gtk_widget_add_css_class(sidebar_, "ifm-sidebar");

    GtkWidget *sidebar_scroll = gtk_scrolled_window_new();
    sidebar_scroll_ = sidebar_scroll;
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sidebar_scroll), sidebar_);
    gtk_paned_set_start_child(GTK_PANED(paned), sidebar_scroll);
    gtk_paned_set_position(GTK_PANED(paned), 228);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);

    GtkWidget *places_header = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(places_header), FALSE);
    GtkWidget *places_label = gtk_label_new("PLACES");
    gtk_widget_add_css_class(places_label, "ifm-section-caption");
    gtk_widget_set_margin_start(places_label, 18);
    gtk_widget_set_margin_top(places_label, 12);
    gtk_widget_set_margin_bottom(places_label, 5);
    gtk_label_set_xalign(GTK_LABEL(places_label), 0.0F);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(places_header), places_label);
    gtk_list_box_append(GTK_LIST_BOX(sidebar_), places_header);
    ++static_sidebar_row_count_;
    add_sidebar_location("Home", "user-home-symbolic", g_get_home_dir());
    if (const char *desktop = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP); desktop != nullptr) {
        add_sidebar_location("Desktop", "user-desktop-symbolic", desktop);
    }
    if (const char *documents = g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS); documents != nullptr) {
        add_sidebar_location("Documents", "folder-documents-symbolic", documents);
    }
    if (const char *downloads = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD); downloads != nullptr) {
        add_sidebar_location("Downloads", "folder-download-symbolic", downloads);
    }
    add_sidebar_location("Computer", "drive-harddisk-symbolic", "/");
    add_sidebar_location("Trash", "user-trash-symbolic", "trash:///");
    GtkWidget *recent_row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(recent_row), FALSE);
    recent_menu_button_ = gtk_menu_button_new();
    gtk_menu_button_set_child(GTK_MENU_BUTTON(recent_menu_button_),
                              make_sidebar_content("Recent", "document-open-recent-symbolic"));
    gtk_widget_set_halign(recent_menu_button_, GTK_ALIGN_FILL);
    gtk_widget_add_css_class(recent_menu_button_, "flat");
    // Recent is a menu button, not an ordinary sidebar row. Its native inner
    // button needs scoped Day/Night styling independent of the host GTK theme.
    gtk_widget_add_css_class(recent_menu_button_, "ifm-recent-button");
    recent_popover_ = gtk_popover_new();
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(recent_menu_button_), recent_popover_);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(recent_row), recent_menu_button_);
    gtk_list_box_append(GTK_LIST_BOX(sidebar_), recent_row);
    ++static_sidebar_row_count_;
    rebuild_recent_menu();
    mounted_places_monitor_ = std::make_unique<MountedPlacesMonitor>([this]() {
        refresh_mounted_places();
    });
    refresh_mounted_places();
    g_signal_connect(sidebar_, "row-activated", G_CALLBACK(on_sidebar_row_activated), this);

    directory_list_ = gtk_directory_list_new(kDirectoryAttributes, nullptr);
    gtk_directory_list_set_monitored(directory_list_, TRUE);
    g_signal_connect(directory_list_, "notify::loading", G_CALLBACK(on_loading_changed), this);
    g_signal_connect(directory_list_, "notify::error", G_CALLBACK(on_loading_changed), this);

    filter_state_ = new FilterState();
    visibility_filter_ = GTK_CUSTOM_FILTER(gtk_custom_filter_new(
        filter_visible, filter_state_,
        [](gpointer data) { delete static_cast<FilterState *>(data); }));
    g_object_ref(visibility_filter_);
    GtkFilterListModel *filtered = gtk_filter_list_model_new(
        G_LIST_MODEL(g_object_ref(directory_list_)), GTK_FILTER(visibility_filter_));
    GtkSortListModel *sort_model =
        gtk_sort_list_model_new(G_LIST_MODEL(filtered), nullptr);
    gtk_sort_list_model_set_incremental(sort_model, TRUE);
    // gtk_multi_selection_new() takes ownership of the sort-model reference.
    selection_ = gtk_multi_selection_new(G_LIST_MODEL(sort_model));

    // Single-item operation controllers still use the authoritative directory
    // model directly. Selection synchronization maps by GFileInfo identity/name
    // so presentation sorting can never redirect an operation to another item.
    primary_selection_ = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(directory_list_)));
    gtk_single_selection_set_autoselect(primary_selection_, FALSE);
    g_signal_connect(selection_, "selection-changed",
                     G_CALLBACK(on_multi_selection_changed), this);
    g_signal_connect(primary_selection_, "notify::selected",
                     G_CALLBACK(on_primary_selection_changed), this);

    GtkWidget *list = gtk_column_view_new(GTK_SELECTION_MODEL(g_object_ref(selection_)));
    detail_view_ = GTK_COLUMN_VIEW(list);
    gtk_column_view_set_single_click_activate(GTK_COLUMN_VIEW(list), FALSE);
    gtk_column_view_set_enable_rubberband(GTK_COLUMN_VIEW(list), TRUE);
    gtk_column_view_set_reorderable(GTK_COLUMN_VIEW(list), TRUE);
    gtk_widget_add_css_class(list, "ifm-file-list");
    g_signal_connect(list, "activate", G_CALLBACK(on_list_activate), this);

    const auto append_detail_column = [this, list](const char *title,
                                                    const DetailField field,
                                                    const int fixed_width,
                                                    const bool expand) {
        GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
        g_object_set_data(G_OBJECT(factory), "ifm-detail-field",
                          GINT_TO_POINTER(static_cast<int>(field)));
        g_signal_connect(factory, "setup", G_CALLBACK(on_factory_setup), this);
        g_signal_connect(factory, "bind", G_CALLBACK(on_factory_bind), this);
        g_signal_connect(factory, "unbind", G_CALLBACK(on_factory_unbind), this);

        GtkColumnViewColumn *column = gtk_column_view_column_new(title, factory);
        gtk_column_view_column_set_expand(column, expand);
        gtk_column_view_column_set_resizable(column, TRUE);
        if (fixed_width > 0) {
            gtk_column_view_column_set_fixed_width(column, fixed_width);
        }

        GtkSorter *sorter = GTK_SORTER(gtk_custom_sorter_new(
            compare_detail_items, new DetailSortContext{folders_first_state_, field},
            [](gpointer data) { delete static_cast<DetailSortContext *>(data); }));
        gtk_column_view_column_set_sorter(column, sorter);
        g_object_unref(sorter);

        gtk_column_view_append_column(GTK_COLUMN_VIEW(list), column);
        g_object_unref(column);
    };

    append_detail_column("Name", DetailField::Name, 0, true);
    append_detail_column("Type", DetailField::Type, kDetailTypeWidth, false);
    append_detail_column("Size", DetailField::Size, kDetailSizeWidth, false);
    append_detail_column("Date Modified", DetailField::Modified, kDetailModifiedWidth, false);

    // GtkColumnView owns the active column/direction state in its aggregate
    // sorter. Feeding that sorter into the shared presentation model makes a
    // header click reorder List, Visual and Compact consistently.
    GListModel *presentation_model = gtk_multi_selection_get_model(selection_);
    if (GTK_IS_SORT_LIST_MODEL(presentation_model)) {
        gtk_sort_list_model_set_sorter(
            GTK_SORT_LIST_MODEL(presentation_model),
            gtk_column_view_get_sorter(GTK_COLUMN_VIEW(list)));
        GListModel *columns = gtk_column_view_get_columns(GTK_COLUMN_VIEW(list));
        auto *name_column = GTK_COLUMN_VIEW_COLUMN(g_list_model_get_item(columns, 0));
        if (name_column != nullptr) {
            gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(list), name_column, GTK_SORT_ASCENDING);
            g_object_unref(name_column);
        }
    }

    GtkListItemFactory *icon_factory = gtk_signal_list_item_factory_new();
    g_object_set_data(G_OBJECT(icon_factory), "ifm-icon-size", GINT_TO_POINTER(64));
    g_object_set_data(G_OBJECT(icon_factory), "ifm-tile-width", GINT_TO_POINTER(132));
    g_signal_connect(icon_factory, "setup", G_CALLBACK(on_icon_factory_setup), this);
    g_signal_connect(icon_factory, "bind", G_CALLBACK(on_icon_factory_bind), this);
    g_signal_connect(icon_factory, "unbind", G_CALLBACK(on_icon_factory_unbind), this);

    // Each view consumes its own reference. The window retains the original
    // selection reference so callback teardown never depends on view disposal order.
    GtkWidget *icons = gtk_grid_view_new(
        GTK_SELECTION_MODEL(g_object_ref(selection_)), icon_factory);
    gtk_grid_view_set_single_click_activate(GTK_GRID_VIEW(icons), FALSE);
    gtk_grid_view_set_enable_rubberband(GTK_GRID_VIEW(icons), TRUE);
    gtk_grid_view_set_max_columns(GTK_GRID_VIEW(icons), 64U);
    gtk_widget_add_css_class(icons, "ifm-icon-grid");
    g_signal_connect(icons, "activate", G_CALLBACK(on_grid_activate), this);

    GtkListItemFactory *compact_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(compact_factory, "setup", G_CALLBACK(on_compact_factory_setup), this);
    g_signal_connect(compact_factory, "bind", G_CALLBACK(on_icon_factory_bind), this);
    g_signal_connect(compact_factory, "unbind", G_CALLBACK(on_icon_factory_unbind), this);

    GtkWidget *compact = gtk_grid_view_new(
        GTK_SELECTION_MODEL(g_object_ref(selection_)), compact_factory);
    gtk_grid_view_set_single_click_activate(GTK_GRID_VIEW(compact), FALSE);
    gtk_grid_view_set_enable_rubberband(GTK_GRID_VIEW(compact), TRUE);
    gtk_grid_view_set_max_columns(GTK_GRID_VIEW(compact), 64U);
    gtk_widget_add_css_class(compact, "ifm-icon-grid");
    gtk_widget_add_css_class(compact, "ifm-compact-grid");
    g_signal_connect(compact, "activate", G_CALLBACK(on_grid_activate), this);

    content_stack_ = gtk_stack_new();
    gtk_widget_add_css_class(content_stack_, "ifm-content-stack");
    gtk_stack_set_transition_type(GTK_STACK(content_stack_), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_stack_add_named(GTK_STACK(content_stack_), make_scroller(list), "list");
    gtk_stack_add_named(GTK_STACK(content_stack_), make_scroller(icons), "icons");
    gtk_stack_add_named(GTK_STACK(content_stack_), make_scroller(compact), "compact");
    GtkWidget *content_overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(content_overlay), content_stack_);
    empty_state_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(empty_state_, "ifm-empty-state");
    gtk_widget_set_halign(empty_state_, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(empty_state_, GTK_ALIGN_CENTER);
    gtk_widget_set_can_target(empty_state_, FALSE);
    GtkWidget *empty_icon = gtk_image_new_from_icon_name("folder-open-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(empty_icon), 42);
    gtk_widget_add_css_class(empty_icon, "dim-label");
    gtk_box_append(GTK_BOX(empty_state_), empty_icon);
    GtkWidget *empty_title = gtk_label_new("This folder is empty");
    empty_title_ = empty_title;
    gtk_widget_add_css_class(empty_title, "title-3");
    gtk_box_append(GTK_BOX(empty_state_), empty_title);
    GtkWidget *empty_detail = gtk_label_new("Create a folder or drop files here");
    gtk_widget_add_css_class(empty_detail, "dim-label");
    gtk_box_append(GTK_BOX(empty_state_), empty_detail);
    gtk_widget_set_visible(empty_state_, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(content_overlay), empty_state_);
    gtk_paned_set_end_child(GTK_PANED(paned), content_overlay);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(list_view_button_), TRUE);

    GtkWidget *status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(status, 0);
    gtk_widget_set_margin_end(status, 0);
    gtk_widget_set_margin_top(status, 0);
    gtk_widget_set_margin_bottom(status, 0);
    gtk_widget_add_css_class(status, "ifm-status");

    spinner_ = gtk_spinner_new();
    gtk_widget_set_visible(spinner_, FALSE);
    gtk_box_append(GTK_BOX(status), spinner_);
    gtk_widget_set_margin_start(spinner_, 14);

    status_label_ = gtk_label_new("");
    gtk_widget_add_css_class(status_label_, "ifm-status-text");
    gtk_label_set_xalign(GTK_LABEL(status_label_), 0.0F);
    gtk_widget_set_margin_start(status_label_, 16);
    gtk_widget_set_hexpand(status_label_, TRUE);
    gtk_box_append(GTK_BOX(status), status_label_);
    gtk_widget_set_margin_end(view_switcher, 12);
    gtk_box_append(GTK_BOX(status), view_switcher);
    gtk_box_append(GTK_BOX(root), status);
}


gboolean FileManagerWindow::filter_visible(gpointer item, gpointer user_data)
{
    const auto *state = static_cast<const FilterState *>(user_data);
    auto *info = G_FILE_INFO(item);
    if (state == nullptr || info == nullptr) return FALSE;
    if (!state->show_hidden && g_file_info_get_is_hidden(info) &&
        std::find(state->revealed_names.begin(), state->revealed_names.end(),
                  g_file_info_get_name(info) != nullptr ? g_file_info_get_name(info) : "") ==
            state->revealed_names.end()) return FALSE;
    if (state->query.empty()) return TRUE;
    const char *name = g_file_info_get_display_name(info);
    if (name == nullptr) name = g_file_info_get_name(info);
    if (name == nullptr) return FALSE;
    char *folded = g_utf8_casefold(name, -1);
    const bool match = folded != nullptr &&
                       std::string(folded).find(state->query) != std::string::npos;
    g_free(folded);
    return match;
}

void FileManagerWindow::show_search()
{
    if (search_entry_ == nullptr) return;
    gtk_widget_set_visible(search_entry_, TRUE);
    gtk_widget_grab_focus(search_entry_);
}
void FileManagerWindow::hide_search()
{
    if (search_entry_ == nullptr) return;
    if (search_timeout_ != 0U) {
        g_source_remove(search_timeout_);
        search_timeout_ = 0U;
    }
    if (filter_state_ != nullptr && !filter_state_->query.empty()) {
        filter_state_->query.clear();
        if (visibility_filter_ != nullptr)
            gtk_filter_changed(GTK_FILTER(visibility_filter_), GTK_FILTER_CHANGE_DIFFERENT);
    }
    if (*gtk_editable_get_text(GTK_EDITABLE(search_entry_)) != '\0')
        gtk_editable_set_text(GTK_EDITABLE(search_entry_), "");
    gtk_widget_set_visible(search_entry_, FALSE);
}
void FileManagerWindow::on_search_toggle(GtkButton *, gpointer user_data)
{
    auto *self = static_cast<FileManagerWindow *>(user_data);
    if (gtk_widget_get_visible(self->search_entry_)) self->hide_search();
    else self->show_search();
    self->update_status();
}
void FileManagerWindow::on_search_changed(GtkSearchEntry *, gpointer user_data)
{
    auto *self = static_cast<FileManagerWindow *>(user_data);
    if (self->search_timeout_ != 0U) g_source_remove(self->search_timeout_);
    self->search_timeout_ = g_timeout_add(160, on_search_timeout, self);
}
gboolean FileManagerWindow::on_search_timeout(gpointer user_data)
{
    auto *self = static_cast<FileManagerWindow *>(user_data);
    self->search_timeout_ = 0U;
    if (self->filter_state_ == nullptr || self->search_entry_ == nullptr) return G_SOURCE_REMOVE;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(self->search_entry_));
    if (text == nullptr || !g_utf8_validate(text, -1, nullptr)) text = "";
    // A bounded current-directory filter, not a recursive or indexed search.
    char *limited = g_utf8_substring(text, 0, MIN(g_utf8_strlen(text, -1), 160));
    char *folded = g_utf8_casefold(limited != nullptr ? limited : "", -1);
    self->filter_state_->query = folded != nullptr ? folded : "";
    g_free(limited);
    g_free(folded);
    gtk_filter_changed(GTK_FILTER(self->visibility_filter_), GTK_FILTER_CHANGE_DIFFERENT);
    self->update_status();
    return G_SOURCE_REMOVE;
}

void FileManagerWindow::load_recents()
{
    char *path = g_build_filename(g_get_user_state_dir(), "infiltrator", "files",
                                  "recent-locations", nullptr);
    char *content = nullptr;
    gsize length = 0;
    if (g_file_get_contents(path, &content, &length, nullptr) && length <= 32U * 1024U) {
        gchar **lines = g_strsplit(content, "\n", 0);
        for (gchar **line = lines; *line != nullptr && recent_locations_.size() < 20U; ++line) {
            if (**line == '\0') continue;
            GFile *file = g_file_new_for_uri(*line);
            if (g_file_is_native(file)) {
                char *native = g_file_get_path(file);
                // Never expose private/hidden locations or system internals.
                const char *home = g_get_home_dir();
                if (native != nullptr && g_str_has_prefix(native, home) &&
                    native[strlen(home)] == '/' &&
                    std::string(native + strlen(home) + 1).find("/.") == std::string::npos &&
                    native[strlen(home) + 1] != '.') {
                    recent_locations_.emplace_back(*line);
                }
                g_free(native);
            }
            g_object_unref(file);
        }
        g_strfreev(lines);
    }
    g_free(content);
    g_free(path);
    rebuild_recent_menu();
}
void FileManagerWindow::save_recents() const
{
    char *dir = g_build_filename(g_get_user_state_dir(), "infiltrator", "files", nullptr);
    if (g_mkdir_with_parents(dir, 0700) == 0) {
        char *path = g_build_filename(dir, "recent-locations", nullptr);
        std::string content;
        for (const auto &uri : recent_locations_) content += uri + "\n";
        g_file_set_contents(path, content.c_str(), static_cast<gssize>(content.size()), nullptr);
        g_free(path);
    }
    g_free(dir);
}
void FileManagerWindow::record_recent(GFile *file)
{
    if (file == nullptr || !g_file_is_native(file)) return;
    char *path = g_file_get_path(file);
    char *uri = g_file_get_uri(file);
    const char *home = g_get_home_dir();
    if (path != nullptr && uri != nullptr && g_str_has_prefix(path, home) &&
        path[strlen(home)] == '/' &&
        path[strlen(home) + 1] != '.' &&
        std::string(path + strlen(home) + 1).find("/.") == std::string::npos) {
        recent_locations_.erase(
            std::remove(recent_locations_.begin(), recent_locations_.end(), uri),
            recent_locations_.end());
        recent_locations_.insert(recent_locations_.begin(), uri);
        if (recent_locations_.size() > 20U) recent_locations_.resize(20U);
        save_recents();
        rebuild_recent_menu();
    }
    g_free(path);
    g_free(uri);
}
void FileManagerWindow::rebuild_recent_menu()
{
    if (recent_popover_ == nullptr) return;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    for (const auto &uri : recent_locations_) {
        GFile *file = g_file_new_for_uri(uri.c_str());
        char *name = g_file_get_parse_name(file);
        GtkWidget *button = gtk_button_new_with_label(name != nullptr ? name : uri.c_str());
        gtk_widget_add_css_class(button, "flat");
        gtk_widget_set_halign(button, GTK_ALIGN_FILL);
        g_object_set_data_full(G_OBJECT(button), "recent-uri", g_strdup(uri.c_str()), g_free);
        g_signal_connect(button, "clicked", G_CALLBACK(on_recent_open), this);
        gtk_box_append(GTK_BOX(box), button);
        g_free(name);
        g_object_unref(file);
    }
    if (recent_locations_.empty())
        gtk_box_append(GTK_BOX(box), gtk_label_new("No recent folders"));
    GtkWidget *clear = gtk_button_new_with_label("Clear recent locations");
    gtk_widget_set_sensitive(clear, !recent_locations_.empty());
    g_signal_connect(clear, "clicked", G_CALLBACK(on_recent_clear), this);
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(box), clear);
    gtk_popover_set_child(GTK_POPOVER(recent_popover_), box);
}
void FileManagerWindow::on_recent_open(GtkButton *button, gpointer user_data)
{
    auto *self = static_cast<FileManagerWindow *>(user_data);
    const auto *uri = static_cast<const char *>(g_object_get_data(G_OBJECT(button), "recent-uri"));
    if (uri == nullptr) return;
    // Navigation refreshes the Recent popover; own the target independently
    // before the active button's menu can be rebuilt and destroyed.
    std::string target(uri);
    gtk_popover_popdown(GTK_POPOVER(self->recent_popover_));
    GFile *file = g_file_new_for_uri(target.c_str());
    self->navigate_to(file, true);
    g_object_unref(file);
}
void FileManagerWindow::on_recent_clear(GtkButton *, gpointer user_data)
{
    auto *self = static_cast<FileManagerWindow *>(user_data);
    self->recent_locations_.clear();
    self->save_recents();
    self->rebuild_recent_menu();
    gtk_popover_popdown(GTK_POPOVER(self->recent_popover_));
}

void FileManagerWindow::load_preferences()
{
    char *path = g_build_filename(g_get_user_config_dir(), "infiltrator", "files",
                                  "preferences.ini", nullptr);
    GKeyFile *keys = g_key_file_new();
    if (g_key_file_load_from_file(keys, path, G_KEY_FILE_NONE, nullptr)) {
        if (g_key_file_has_key(keys, "Appearance", "show-hidden", nullptr))
            show_hidden_ = g_key_file_get_boolean(keys, "Appearance", "show-hidden", nullptr);
        if (g_key_file_has_key(keys, "Appearance", "sidebar-visible", nullptr))
            sidebar_visible_ = g_key_file_get_boolean(keys, "Appearance", "sidebar-visible", nullptr);
        if (g_key_file_has_key(keys, "Appearance", "folders-first", nullptr))
            folders_first_ = g_key_file_get_boolean(keys, "Appearance", "folders-first", nullptr);
        const gint mode = g_key_file_get_integer(keys, "Appearance", "view", nullptr);
        if (mode >= 1 && mode <= 3)
            view_mode_ = static_cast<ViewMode>(mode - 1);
    }
    g_key_file_unref(keys);
    g_free(path);
    if (filter_state_ != nullptr) filter_state_->show_hidden = show_hidden_;
    folders_first_state_->store(folders_first_, std::memory_order_relaxed);
    // Loading settings must not create config directories as a side effect.
}

void FileManagerWindow::save_preferences() const
{
    if (!preferences_ready_) return;
    char *folder = g_build_filename(g_get_user_config_dir(), "infiltrator", "files", nullptr);
    if (g_mkdir_with_parents(folder, 0700) != 0) {
        g_free(folder);
        return;
    }
    char *path = g_build_filename(folder, "preferences.ini", nullptr);
    GKeyFile *keys = g_key_file_new();
    g_key_file_set_boolean(keys, "Appearance", "show-hidden", show_hidden_);
    g_key_file_set_boolean(keys, "Appearance", "sidebar-visible", sidebar_visible_);
    g_key_file_set_boolean(keys, "Appearance", "folders-first", folders_first_);
    g_key_file_set_integer(keys, "Appearance", "view", static_cast<gint>(view_mode_) + 1);
    gsize bytes = 0;
    char *content = g_key_file_to_data(keys, &bytes, nullptr);
    if (content != nullptr) {
        // GLib writes via temporary-file replacement, never a partially-written preference.
        g_file_set_contents(path, content, static_cast<gssize>(bytes), nullptr);
    }
    g_free(content);
    g_key_file_unref(keys);
    g_free(path);
    g_free(folder);
}

void FileManagerWindow::apply_preferences()
{
    if (visibility_filter_ != nullptr)
        gtk_filter_changed(GTK_FILTER(visibility_filter_), GTK_FILTER_CHANGE_DIFFERENT);
    if (sidebar_scroll_ != nullptr)
        gtk_widget_set_visible(sidebar_scroll_, sidebar_visible_);
    set_view_mode(view_mode_);
    preferences_ready_ = true;
}

void FileManagerWindow::notify_sort_changed()
{
    if (detail_view_ == nullptr) return;
    GListModel *columns = gtk_column_view_get_columns(detail_view_);
    for (guint i = 0; i < g_list_model_get_n_items(columns); ++i) {
        auto *column = GTK_COLUMN_VIEW_COLUMN(g_list_model_get_item(columns, i));
        if (column != nullptr) {
            GtkSorter *sorter = gtk_column_view_column_get_sorter(column);
            if (sorter != nullptr) gtk_sorter_changed(sorter, GTK_SORTER_CHANGE_DIFFERENT);
            g_object_unref(column);
        }
    }
}

void FileManagerWindow::toggle_show_hidden()
{
    show_hidden_ = !show_hidden_;
    if (filter_state_ != nullptr) filter_state_->show_hidden = show_hidden_;
    if (visibility_filter_ != nullptr)
        gtk_filter_changed(GTK_FILTER(visibility_filter_), GTK_FILTER_CHANGE_DIFFERENT);
    save_preferences();
    update_status();
}

void FileManagerWindow::toggle_folders_first()
{
    folders_first_ = !folders_first_;
    folders_first_state_->store(folders_first_, std::memory_order_relaxed);
    notify_sort_changed();
    save_preferences();
}

void FileManagerWindow::toggle_sidebar()
{
    sidebar_visible_ = !sidebar_visible_;
    if (sidebar_scroll_ != nullptr)
        gtk_widget_set_visible(sidebar_scroll_, sidebar_visible_);
    save_preferences();
}

void FileManagerWindow::open_new_window(bool selected_folder)
{
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(window_));
    if (app == nullptr) return;
    GFile *location = current_location_ != nullptr ? G_FILE(g_object_ref(current_location_))
                                                   : g_file_new_for_path(g_get_home_dir());
    if (selected_folder && primary_selection_ != nullptr) {
        auto *info = G_FILE_INFO(gtk_single_selection_get_selected_item(primary_selection_));
        if (info == nullptr || g_file_info_get_file_type(info) != G_FILE_TYPE_DIRECTORY) {
            g_object_unref(location);
            return;
        }
        const char *name = g_file_info_get_name(info);
        if (name == nullptr) { g_object_unref(location); return; }
        GFile *child = g_file_get_child(location, name);
        g_object_unref(location);
        location = child;
    }
    auto *created = new FileManagerWindow(app);
    created->install_operation_surface();
    new ThemeController(created->native_window());
    created->navigate_to(location, true);
    created->present();
    g_object_unref(location);
}

bool FileManagerWindow::open_primary_directory_in_new_window()
{
    if (primary_selection_ == nullptr) return false;
    auto *info = G_FILE_INFO(gtk_single_selection_get_selected_item(primary_selection_));
    if (info == nullptr || g_file_info_get_file_type(info) != G_FILE_TYPE_DIRECTORY)
        return false;
    open_new_window(true);
    return true;
}

void FileManagerWindow::duplicate_active_tab()
{
    if (active_tab_ >= tabs_.size() || tabs_.size() >= 128U) return;
    remember_tab();
    TabState duplicate = tabs_[active_tab_];
    duplicate.id = next_tab_id();
    tabs_.push_back(std::move(duplicate));
    active_tab_ = tabs_.size() - 1U;
    display_active_tab();
}

void FileManagerWindow::reopen_closed_tab()
{
    if (recently_closed_tabs_.empty() || tabs_.size() >= 128U) return;
    remember_tab();
    TabState restored = std::move(recently_closed_tabs_.back());
    recently_closed_tabs_.pop_back();
    restored.id = next_tab_id();
    tabs_.push_back(std::move(restored));
    active_tab_ = tabs_.size() - 1U;
    display_active_tab();
}

void FileManagerWindow::move_active_tab(int delta)
{
    if (tabs_.size() < 2U || (delta != -1 && delta != 1)) return;
    const std::ptrdiff_t destination = static_cast<std::ptrdiff_t>(active_tab_) + delta;
    if (destination < 0 || destination >= static_cast<std::ptrdiff_t>(tabs_.size())) return;
    remember_tab();
    std::swap(tabs_[active_tab_], tabs_[static_cast<std::size_t>(destination)]);
    active_tab_ = static_cast<std::size_t>(destination);
    update_tab_buttons();
}

void FileManagerWindow::update_sidebar_current_state()
{
    if (sidebar_ == nullptr || current_location_ == nullptr) return;
    for (GtkWidget *row = gtk_widget_get_first_child(sidebar_);
         row != nullptr; row = gtk_widget_get_next_sibling(row)) {
        const char *target = static_cast<const char *>(
            g_object_get_data(G_OBJECT(row), "ifm-target"));
        bool current = false;
        if (target != nullptr) {
            GFile *file = file_from_location_text(target);
            if (file != nullptr) {
                current = g_file_equal(file, current_location_) != FALSE;
                g_object_unref(file);
            }
        }
        if (current) gtk_widget_add_css_class(row, "ifm-place-current");
        else gtk_widget_remove_css_class(row, "ifm-place-current");
    }
}

void FileManagerWindow::add_sidebar_location(const char *title, const char *icon_name, const char *target)
{
    if (target == nullptr || target[0] == '\0') {
        return;
    }

    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), make_sidebar_content(title, icon_name));
    g_object_set_data_full(G_OBJECT(row), "ifm-target", g_strdup(target), g_free);
    gtk_list_box_append(GTK_LIST_BOX(sidebar_), row);
    ++static_sidebar_row_count_;
}

void FileManagerWindow::refresh_mounted_places()
{
    if (mounted_places_monitor_ == nullptr) {
        return;
    }

    apply_mounted_places(mounted_places_monitor_->snapshot());
}

void FileManagerWindow::apply_mounted_places(const std::vector<MountedPlace> &next_places)
{
    std::string current_uri;
    if (current_location_ != nullptr) {
        char *uri = g_file_get_uri(current_location_);
        if (uri != nullptr) {
            current_uri = uri;
            g_free(uri);
        }
    }

    const MountedPlace *previous_source =
        most_specific_mounted_place_for_location(current_uri, mounted_places_);
    std::optional<MountedPlace> previous_source_copy;
    if (previous_source != nullptr) {
        previous_source_copy = *previous_source;
    }
    const MountedPlace *next_source =
        most_specific_mounted_place_for_location(current_uri, next_places);

    reconcile_mounted_place_rows(next_places);
    mounted_places_ = next_places;

    if (current_location_available_) {
        if (previous_source_copy.has_value() &&
            (next_source == nullptr ||
             !mounted_places_have_same_source(*previous_source_copy, *next_source))) {
            mark_current_location_unavailable(*previous_source_copy);
        }
        return;
    }

    restore_current_location_if_proven(next_source);
}

void FileManagerWindow::reconcile_mounted_place_rows(const std::vector<MountedPlace> &places)
{
    for (auto it = mounted_place_rows_.begin(); it != mounted_place_rows_.end();) {
        const bool still_present = std::any_of(places.begin(), places.end(),
                                               [&it](const MountedPlace &place) {
                                                   return mounted_places_have_same_target(
                                                       it->place, place);
                                               });
        if (still_present) {
            ++it;
            continue;
        }
        gtk_list_box_remove(GTK_LIST_BOX(sidebar_), it->row);
        it = mounted_place_rows_.erase(it);
    }

    for (const MountedPlace &place : places) {
        auto existing = std::find_if(mounted_place_rows_.begin(), mounted_place_rows_.end(),
                                     [&place](const MountedPlaceRow &entry) {
                                         return mounted_places_have_same_target(entry.place, place);
                                     });
        if (existing != mounted_place_rows_.end()) {
            if (existing->place.name != place.name ||
                existing->place.removable != place.removable) {
                const char *icon_name = place.removable ? "drive-removable-media-symbolic"
                                                        : "drive-harddisk-symbolic";
                gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(existing->row),
                                           make_sidebar_content(place.name.c_str(), icon_name));
            }
            existing->place = place;
            continue;
        }

        GtkWidget *row = gtk_list_box_row_new();
        const char *icon_name = place.removable ? "drive-removable-media-symbolic"
                                                : "drive-harddisk-symbolic";
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row),
                                   make_sidebar_content(place.name.c_str(), icon_name));
        g_object_set_data_full(G_OBJECT(row), "ifm-target", g_strdup(place.uri.c_str()), g_free);
        gtk_list_box_append(GTK_LIST_BOX(sidebar_), row);
        mounted_place_rows_.push_back(MountedPlaceRow{place, row});
    }

    std::vector<MountedPlaceRow> ordered_rows;
    ordered_rows.reserve(places.size());
    for (const MountedPlace &place : places) {
        auto existing = std::find_if(mounted_place_rows_.begin(), mounted_place_rows_.end(),
                                     [&place](const MountedPlaceRow &entry) {
                                         return mounted_places_have_same_target(entry.place, place);
                                     });
        if (existing != mounted_place_rows_.end()) {
            ordered_rows.push_back(*existing);
        }
    }
    mounted_place_rows_.swap(ordered_rows);

    for (std::size_t index = 0U; index < mounted_place_rows_.size(); ++index) {
        GtkWidget *row = mounted_place_rows_[index].row;
        const int expected_index = static_cast<int>(static_sidebar_row_count_ + index);
        if (gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(row)) == expected_index) {
            continue;
        }

        const bool had_focus = gtk_widget_has_focus(row);
        // Keep a strong reference while reparenting so ordering changes preserve
        // the GtkListBoxRow identity used by keyboard and assistive-technology state.
        g_object_ref(row);
        gtk_list_box_remove(GTK_LIST_BOX(sidebar_), row);
        gtk_list_box_insert(GTK_LIST_BOX(sidebar_), row, expected_index);
        if (had_focus) {
            gtk_widget_grab_focus(row);
        }
        g_object_unref(row);
    }
}

void FileManagerWindow::mark_current_location_unavailable(const MountedPlace &source)
{
    if (!current_location_available_) {
        return;
    }

    current_location_available_ = false;
    unavailable_mounted_place_ = source;
    if (selection_ != nullptr) {
        gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(selection_));
    }
    if (directory_list_ != nullptr) {
        gtk_directory_list_set_file(directory_list_, nullptr);
    }
    if (spinner_ != nullptr) {
        gtk_spinner_stop(GTK_SPINNER(spinner_));
        gtk_widget_set_visible(spinner_, FALSE);
    }
    if (status_label_ != nullptr) {
        gtk_label_set_text(GTK_LABEL(status_label_), kLocationUnavailableStatus);
    }
    update_navigation_state();
}

void FileManagerWindow::restore_current_location_if_proven(const MountedPlace *source)
{
    if (current_location_available_ || source == nullptr ||
        !unavailable_mounted_place_.has_value() || current_location_ == nullptr ||
        directory_list_ == nullptr) {
        return;
    }
    if (!mounted_place_reappearance_is_proven(*unavailable_mounted_place_, *source)) {
        return;
    }

    current_location_available_ = true;
    unavailable_mounted_place_.reset();
    if (selection_ != nullptr) {
        gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(selection_));
    }
    gtk_directory_list_set_file(directory_list_, current_location_);
    update_navigation_state();
    update_status();
}

void FileManagerWindow::navigate_to(GFile *file, const bool record_history)
{
    if (file == nullptr) {
        return;
    }

    ++navigation_generation_;
    if (location_request_ != nullptr) g_cancellable_cancel(location_request_);
    g_clear_object(&location_request_);
    if (filter_state_ != nullptr && !filter_state_->revealed_names.empty()) {
        filter_state_->revealed_names.clear();
        gtk_filter_changed(GTK_FILTER(visibility_filter_), GTK_FILTER_CHANGE_DIFFERENT);
    }

    char *uri_text = g_file_get_uri(file);
    char *parse_name = g_file_get_parse_name(file);
    if (uri_text == nullptr || parse_name == nullptr) {
        g_free(uri_text);
        g_free(parse_name);
        return;
    }

    Location location{uri_text, parse_name, classify_location_uri(uri_text)};

    if (record_history) {
        const bool duplicate_current = !history_.empty() && history_index_ < history_.size() &&
                                       history_[history_index_].uri == location.uri;
        if (!duplicate_current) {
            if (!history_.empty() && history_index_ + 1U < history_.size()) {
                history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(history_index_ + 1U),
                               history_.end());
            }
            history_.push_back(location);
            history_index_ = history_.size() - 1U;
        }
    }

    if (current_location_ != nullptr) {
        g_object_unref(current_location_);
    }
    current_location_ = G_FILE(g_object_ref(file));
    current_location_available_ = true;
    unavailable_mounted_place_.reset();

    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(selection_));
    hide_search();
    if (const auto saved = folder_views_.find(location.uri); saved != folder_views_.end())
        set_view_mode(saved->second);
    gtk_directory_list_set_file(directory_list_, file);
    if (record_history) record_recent(file);
    gtk_editable_set_text(GTK_EDITABLE(location_entry_), parse_name);

    const std::string title = location.display_name.empty() ? "Files" : "Files — " + location.display_name;
    gtk_window_set_title(GTK_WINDOW(window_), title.c_str());

    g_free(uri_text);
    g_free(parse_name);
    update_navigation_state();
    update_status();
    update_tab_buttons();
    update_bookmark_button();
    update_sidebar_current_state();
}

void FileManagerWindow::navigate_history(const std::ptrdiff_t delta)
{
    if (history_.empty()) {
        return;
    }

    const std::ptrdiff_t current = static_cast<std::ptrdiff_t>(history_index_);
    const std::ptrdiff_t target = current + delta;
    if (target < 0 || target >= static_cast<std::ptrdiff_t>(history_.size())) {
        return;
    }

    history_index_ = static_cast<std::size_t>(target);
    GFile *file = g_file_new_for_uri(history_[history_index_].uri.c_str());
    navigate_to(file, false);
    g_object_unref(file);
}

void FileManagerWindow::activate_position(const guint position)
{
    if (current_location_ == nullptr || selection_ == nullptr) {
        return;
    }

    GListModel *model = gtk_multi_selection_get_model(selection_);
    GFileInfo *info = model != nullptr
        ? G_FILE_INFO(g_list_model_get_item(model, position)) : nullptr;
    if (info == nullptr) {
        return;
    }

    const char *name = g_file_info_get_name(info);
    if (name == nullptr) {
        g_object_unref(info);
        return;
    }

    GFile *child = g_file_get_child(current_location_, name);
    if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
        navigate_to(child, true);
    } else {
        char *uri = g_file_get_uri(child);
        if (uri != nullptr) {
            g_app_info_launch_default_for_uri_async(uri, nullptr, nullptr,
                                                    &FileManagerWindow::on_launch_finished,
                                                    g_object_ref(status_label_));
            g_free(uri);
        }
    }

    g_object_unref(child);
    g_object_unref(info);
}

void FileManagerWindow::refresh_current_directory()
{
    if (current_location_ == nullptr || !current_location_available_ ||
        directory_list_ == nullptr || !gtk_directory_list_get_monitored(directory_list_))
        return;
    // A refresh re-enumerates the same directory, never navigation history or
    // file mutations. Retain selections by item name until the new snapshot arrives.
    remember_tab();
    if (active_tab_ < tabs_.size()) pending_tab_selection_ = tabs_[active_tab_].selected_names;
    gtk_directory_list_set_file(directory_list_, nullptr);
    gtk_directory_list_set_file(directory_list_, current_location_);
    update_status();
}

void FileManagerWindow::set_view_mode(const ViewMode mode)
{
    if (content_stack_ == nullptr) {
        return;
    }

    view_mode_ = mode;
    if (preferences_ready_) save_preferences();
    GtkWidget *active_button = mode == ViewMode::List ? list_view_button_ :
        mode == ViewMode::Icons ? icon_view_button_ : compact_view_button_;
    if (GTK_IS_TOGGLE_BUTTON(active_button) &&
        !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(active_button))) {
        view_mode_programmatic_ = true;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(active_button), TRUE);
        view_mode_programmatic_ = false;
    }
    switch (mode) {
    case ViewMode::List:
        gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "list");
        break;
    case ViewMode::Icons:
        gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "icons");
        break;
    case ViewMode::Compact:
        gtk_stack_set_visible_child_name(GTK_STACK(content_stack_), "compact");
        break;
    }
}

void FileManagerWindow::update_navigation_state()
{
    gtk_widget_set_sensitive(back_button_, !history_.empty() && history_index_ > 0U);
    gtk_widget_set_sensitive(forward_button_, !history_.empty() && history_index_ + 1U < history_.size());

    bool has_parent = false;
    if (current_location_available_ && current_location_ != nullptr) {
        GFile *parent = g_file_get_parent(current_location_);
        has_parent = parent != nullptr;
        if (parent != nullptr) {
            g_object_unref(parent);
        }
    }
    gtk_widget_set_sensitive(up_button_, has_parent);
}

void FileManagerWindow::update_status()
{
    if (directory_list_ == nullptr) {
        return;
    }

    if (!current_location_available_) {
        if (empty_state_ != nullptr) gtk_widget_set_visible(empty_state_, FALSE);
        gtk_spinner_stop(GTK_SPINNER(spinner_));
        gtk_widget_set_visible(spinner_, FALSE);
        gtk_label_set_text(GTK_LABEL(status_label_), kLocationUnavailableStatus);
        return;
    }

    if (gtk_directory_list_is_loading(directory_list_)) {
        if (empty_state_ != nullptr) gtk_widget_set_visible(empty_state_, FALSE);
        gtk_widget_set_visible(spinner_, TRUE);
        gtk_spinner_start(GTK_SPINNER(spinner_));
        gtk_label_set_text(GTK_LABEL(status_label_), "Loading…");
        return;
    }

    gtk_spinner_stop(GTK_SPINNER(spinner_));
    gtk_widget_set_visible(spinner_, FALSE);

    if (const GError *error = gtk_directory_list_get_error(directory_list_); error != nullptr) {
        if (empty_state_ != nullptr) gtk_widget_set_visible(empty_state_, FALSE);
        gtk_label_set_text(GTK_LABEL(status_label_), error->message);
        return;
    }

    restore_tab_selection();
    GListModel *visible = gtk_multi_selection_get_model(selection_);
    const guint count = visible != nullptr ? g_list_model_get_n_items(visible) : 0U;
    if (empty_state_ != nullptr) gtk_widget_set_visible(empty_state_, count == 0U);
    if (empty_title_ != nullptr) gtk_label_set_text(GTK_LABEL(empty_title_),
        filter_state_ != nullptr && !filter_state_->query.empty()
            ? "No matching files" : "This folder is empty");
    GtkBitset *selected = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(selection_));
    const guint selected_count = static_cast<guint>(gtk_bitset_get_size(selected));
    gtk_bitset_unref(selected);
    std::string text = std::to_string(count) + (count == 1U ? " item" : " items");
    if (selected_count > 0U) text += "  ·  " + std::to_string(selected_count) + " selected";
    gtk_label_set_text(GTK_LABEL(status_label_), text.c_str());
}

void FileManagerWindow::sync_primary_from_multi()
{
    if (selection_syncing_ || selection_ == nullptr || primary_selection_ == nullptr) {
        return;
    }

    selection_syncing_ = true;
    guint selected_position = GTK_INVALID_LIST_POSITION;
    GtkBitset *selected_items = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(selection_));
    GListModel *presentation_model = gtk_multi_selection_get_model(selection_);
    if (presentation_model != nullptr && gtk_bitset_get_size(selected_items) == 1U) {
        const guint index = gtk_bitset_get_minimum(selected_items);
        GFileInfo *selected_info =
            G_FILE_INFO(g_list_model_get_item(presentation_model, index));
        selected_position = find_item_position(
            G_LIST_MODEL(directory_list_), selected_info);
        if (selected_info != nullptr) {
            g_object_unref(selected_info);
        }
    }
    gtk_bitset_unref(selected_items);
    gtk_single_selection_set_selected(primary_selection_, selected_position);
    selection_syncing_ = false;
}

void FileManagerWindow::sync_multi_from_primary()
{
    if (selection_syncing_ || selection_ == nullptr || primary_selection_ == nullptr) {
        return;
    }

    selection_syncing_ = true;
    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(selection_));
    const guint selected = gtk_single_selection_get_selected(primary_selection_);
    if (selected != GTK_INVALID_LIST_POSITION) {
        GFileInfo *selected_info = G_FILE_INFO(
            g_list_model_get_item(G_LIST_MODEL(directory_list_), selected));
        GListModel *presentation_model = gtk_multi_selection_get_model(selection_);
        const guint presentation_position =
            find_item_position(presentation_model, selected_info);
        if (selected_info != nullptr) {
            g_object_unref(selected_info);
        }
        if (presentation_position != GTK_INVALID_LIST_POSITION) {
            gtk_selection_model_select_item(GTK_SELECTION_MODEL(selection_),
                                            presentation_position, TRUE);
        }
    }
    selection_syncing_ = false;
}

void FileManagerWindow::arm_temporal_policy_monitor()
{
    if (temporal_policy_monitor_ != nullptr) {
        g_signal_handlers_disconnect_by_data(temporal_policy_monitor_, this);
        g_object_unref(temporal_policy_monitor_);
        temporal_policy_monitor_ = nullptr;
    }

    char directory[4096]{};
    if (!infiltratr_temporal_posix_policy_directory(directory, sizeof(directory))) {
        return;
    }

    GFile *policy_directory = g_file_new_for_path(directory);
    GFile *target = policy_directory;
    temporal_policy_monitoring_parent_ = false;
    temporal_policy_watch_name_ = "presentation.conf";
    // A fresh profile may lack both the XDG config home and its infiltrator
    // child. Watch the nearest existing ancestor, then move inward as each
    // missing directory is created; never create preferences just to watch them.
    while (target != nullptr) {
        char *path = g_file_get_path(target);
        const bool exists = path != nullptr && g_file_test(path, G_FILE_TEST_IS_DIR);
        g_free(path);
        if (exists) {
            break;
        }
        char *name = g_file_get_basename(target);
        temporal_policy_watch_name_ = name != nullptr ? name : "";
        g_free(name);
        temporal_policy_monitoring_parent_ = true;
        GFile *parent = g_file_get_parent(target);
        g_object_unref(target);
        target = parent;
    }
    if (target == nullptr) {
        return;
    }

    GError *error = nullptr;
    temporal_policy_monitor_ = g_file_monitor_directory(
        target, G_FILE_MONITOR_WATCH_MOVES, nullptr, &error);
    g_object_unref(target);
    if (error != nullptr) {
        g_error_free(error);
    }
    if (temporal_policy_monitor_ != nullptr) {
        g_signal_connect(temporal_policy_monitor_, "changed",
                         G_CALLBACK(on_temporal_policy_changed), this);
    }
}

void FileManagerWindow::refresh_modified_cells()
{
    for (GtkWidget *cell : modified_cells_) {
        auto *info = G_FILE_INFO(
            g_object_get_data(G_OBJECT(cell), "ifm-modified-info"));
        if (info == nullptr) {
            continue;
        }
        const std::string value = detail_modified_text(info);
        gtk_label_set_text(GTK_LABEL(cell), value.c_str());
    }
}

void FileManagerWindow::on_temporal_policy_changed(GFileMonitor *monitor,
                                                   GFile *file,
                                                   GFile *other_file,
                                                   GFileMonitorEvent event_type,
                                                   gpointer user_data)
{
    (void)monitor;
    (void)event_type;
    auto *self = static_cast<FileManagerWindow *>(user_data);
    const auto has_basename = [](GFile *candidate, const char *expected) {
        if (candidate == nullptr) {
            return false;
        }
        char *basename = g_file_get_basename(candidate);
        const bool matches = g_strcmp0(basename, expected) == 0;
        g_free(basename);
        return matches;
    };

    if (self->temporal_policy_monitoring_parent_) {
        if (!has_basename(file, self->temporal_policy_watch_name_.c_str()) &&
            !has_basename(other_file, self->temporal_policy_watch_name_.c_str())) {
            return;
        }
        self->arm_temporal_policy_monitor();
        self->refresh_modified_cells();
        return;
    }

    if (has_basename(file, "presentation.conf") ||
        has_basename(other_file, "presentation.conf")) {
        self->refresh_modified_cells();
    }
}

void FileManagerWindow::on_modified_cell_finalized(gpointer user_data,
                                                   GObject *where_object_was)
{
    auto *self = static_cast<FileManagerWindow *>(user_data);
    self->modified_cells_.erase(GTK_WIDGET(where_object_was));
}

GFile *FileManagerWindow::file_from_location_text(const char *text) const
{
    if (text == nullptr || text[0] == '\0') {
        return nullptr;
    }

    std::string value(text);
    if (value == "~") {
        value = g_get_home_dir();
    } else if (value.rfind("~/", 0U) == 0U) {
        value = std::string(g_get_home_dir()) + value.substr(1U);
    }

    char *scheme = g_uri_parse_scheme(value.c_str());
    if (scheme != nullptr) {
        g_free(scheme);
        return g_file_new_for_uri(value.c_str());
    }
    if (!g_path_is_absolute(value.c_str()) && current_location_ != nullptr)
        return g_file_resolve_relative_path(current_location_, value.c_str());
    return g_file_new_for_commandline_arg(value.c_str());
}


bool FileManagerWindow::open_primary_directory_in_new_tab()
{
    if (current_location_ == nullptr || primary_selection_ == nullptr) return false;
    auto *info = G_FILE_INFO(gtk_single_selection_get_selected_item(primary_selection_));
    if (info == nullptr || g_file_info_get_file_type(info) != G_FILE_TYPE_DIRECTORY) return false;
    const char *name = g_file_info_get_name(info);
    if (name == nullptr || *name == '\0') return false;
    GFile *child = g_file_get_child(current_location_, name);
    add_tab(child);
    g_object_unref(child);
    return true;
}

void FileManagerWindow::remember_tab()
{
    if (active_tab_ >= tabs_.size()) return;
    TabState &tab = tabs_[active_tab_];
    tab.history = history_;
    tab.index = history_index_;
    tab.view = view_mode_;
    if (current_location_ != nullptr) {
        char *uri = g_file_get_uri(current_location_);
        tab.uri = uri != nullptr ? uri : "";
        g_free(uri);
    }
    tab.selected_names.clear();
    if (selection_ == nullptr) return;
    GListModel *model = gtk_multi_selection_get_model(selection_);
    if (model == nullptr) return;
    GtkBitset *bitset = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(selection_));
    GtkBitsetIter iter;
    guint pos = 0;
    for (gboolean valid = gtk_bitset_iter_init_first(&iter, bitset, &pos);
         valid; valid = gtk_bitset_iter_next(&iter, &pos)) {
        auto *info = G_FILE_INFO(g_list_model_get_item(model, pos));
        if (info != nullptr) {
            const char *name = g_file_info_get_name(info);
            if (name != nullptr) tab.selected_names.emplace_back(name);
            g_object_unref(info);
        }
    }
    gtk_bitset_unref(bitset);
}

void FileManagerWindow::restore_tab_selection()
{
    if (pending_tab_selection_.empty() || directory_list_ == nullptr ||
        gtk_directory_list_is_loading(directory_list_)) return;
    if (filter_state_ != nullptr) {
        filter_state_->revealed_names = pending_tab_selection_;
        gtk_filter_changed(GTK_FILTER(visibility_filter_), GTK_FILTER_CHANGE_DIFFERENT);
    }
    GListModel *model = gtk_multi_selection_get_model(selection_);
    if (model == nullptr) return;
    for (guint pos = 0; pos < g_list_model_get_n_items(model); ++pos) {
        auto *info = G_FILE_INFO(g_list_model_get_item(model, pos));
        if (info == nullptr) continue;
        const char *name = g_file_info_get_name(info);
        if (name != nullptr &&
            std::find(pending_tab_selection_.begin(), pending_tab_selection_.end(), name) !=
                pending_tab_selection_.end()) {
            gtk_selection_model_select_item(GTK_SELECTION_MODEL(selection_), pos, FALSE);
        }
        g_object_unref(info);
    }
    pending_tab_selection_.clear();
}

void FileManagerWindow::add_tab(GFile *file)
{
    if (file == nullptr || tabs_.size() >= 128U) return;
    GFile *target = G_FILE(g_object_ref(file));
    remember_tab();
    tabs_.push_back(TabState{});
    tabs_.back().view = view_mode_;
    active_tab_ = tabs_.size() - 1U;
    history_.clear();
    history_index_ = 0;
    pending_tab_selection_.clear();
    navigate_to(target, true);
    g_object_unref(target);
    update_tab_buttons();
}

void FileManagerWindow::activate_tab(const std::size_t index)
{
    if (index >= tabs_.size() || index == active_tab_) return;
    remember_tab();
    active_tab_ = index;
    display_active_tab();
}

void FileManagerWindow::display_active_tab()
{
    if (active_tab_ >= tabs_.size()) return;
    const TabState tab = tabs_[active_tab_];
    history_ = tab.history;
    history_index_ = tab.index;
    pending_tab_selection_ = tab.selected_names;
    if (!tab.uri.empty()) {
        GFile *file = g_file_new_for_uri(tab.uri.c_str());
        navigate_to(file, false);
        g_object_unref(file);
    }
    set_view_mode(tab.view);
    update_tab_buttons();
    restore_tab_selection();
}

void FileManagerWindow::close_tab(const std::size_t index)
{
    remove_tab(index, true);
}

void FileManagerWindow::remove_tab(const std::size_t index, const bool remember_closed)
{
    if (index >= tabs_.size()) return;
    if (tabs_.size() == 1U && remember_closed) {
        (void)save_session();
        tabs_.clear();
        gtk_window_destroy(native_window());
        return;
    }
    remember_tab();
    if (remember_closed) {
        recently_closed_tabs_.push_back(tabs_[index]);
        if (recently_closed_tabs_.size() > 20U) recently_closed_tabs_.erase(recently_closed_tabs_.begin());
    }
    const bool current = index == active_tab_;
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));
    if (tabs_.empty()) {
        gtk_window_destroy(native_window());
        return;
    }
    if (current) {
        active_tab_ = std::min(index, tabs_.size() - 1U);
        display_active_tab();
    } else {
        if (index < active_tab_) --active_tab_;
        update_tab_buttons();
    }
}

void FileManagerWindow::update_tab_buttons()
{
    if (tab_strip_ == nullptr) return;
    // Rebuild is confined to tab controls; the file views and their GTK models
    // retain identity and operation-controller connections across tab changes.
    while (GtkWidget *child = gtk_widget_get_first_child(tab_strip_)) {
        gtk_box_remove(GTK_BOX(tab_strip_), child);
    }
    for (std::size_t index = 0; index < tabs_.size(); ++index) {
        GtkWidget *group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(group, "ifm-tab-group");
        GtkWidget *button = gtk_button_new();
        gtk_widget_set_size_request(button, 84, 32);
        gtk_widget_set_hexpand(button, FALSE);
        gtk_widget_add_css_class(button, "ifm-tab-button");
        if (index == active_tab_) gtk_widget_add_css_class(button, "ifm-tab-active");
        else gtk_widget_add_css_class(button, "flat");
        std::string text = "Home";
        std::string uri = tabs_[index].uri;
        if (index == active_tab_ && current_location_ != nullptr) {
            char *actual = g_file_get_uri(current_location_);
            if (actual != nullptr) uri = actual;
            g_free(actual);
        }
        if (!uri.empty()) {
            GFile *file = g_file_new_for_uri(uri.c_str());
            char *name = g_file_get_basename(file);
            if (name != nullptr && *name != '\0') text = name;
            else text = uri;
            g_free(name);
            g_object_unref(file);
        }
        GtkWidget *label = gtk_label_new(text.c_str());
        gtk_label_set_max_width_chars(GTK_LABEL(label), 18);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
        gtk_button_set_child(GTK_BUTTON(button), label);
        g_object_set_data(G_OBJECT(button), "ifm-tab-index",
                          GINT_TO_POINTER(static_cast<int>(index + 1U)));
        g_signal_connect(button, "clicked", G_CALLBACK(on_tab_clicked), this);
        install_tab_drag(button, tabs_[index].id);
        gtk_box_append(GTK_BOX(group), button);
        GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
        gtk_widget_set_tooltip_text(close, "Close tab (Ctrl+W)");
        gtk_widget_add_css_class(close, "flat");
        gtk_widget_add_css_class(close, "ifm-tab-close");
        gtk_widget_set_opacity(close, index == active_tab_ ? 0.95 : 0.68);
        g_object_set_data(G_OBJECT(close), "ifm-tab-index",
                          GINT_TO_POINTER(static_cast<int>(index + 1U)));
        g_signal_connect(close, "clicked", G_CALLBACK(on_tab_close_clicked), this);
        gtk_box_append(GTK_BOX(group), close);
        gtk_box_append(GTK_BOX(tab_strip_), group);
    }
}

void FileManagerWindow::on_tab_clicked(GtkButton *button, gpointer data)
{
    auto *self = static_cast<FileManagerWindow *>(data);
    const int encoded = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "ifm-tab-index"));
    if (encoded > 0) self->activate_tab(static_cast<std::size_t>(encoded - 1));
}
void FileManagerWindow::on_tab_close_clicked(GtkButton *button, gpointer data)
{
    auto *self = static_cast<FileManagerWindow *>(data);
    const int encoded = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "ifm-tab-index"));
    if (encoded > 0) self->close_tab(static_cast<std::size_t>(encoded - 1));
}
void FileManagerWindow::on_tab_add_clicked(GtkButton *, gpointer data)
{
    auto *self = static_cast<FileManagerWindow *>(data);
    if (self->current_location_ != nullptr) self->add_tab(self->current_location_);
}
gboolean FileManagerWindow::on_tab_key_pressed(GtkEventControllerKey *, guint keyval,
                                                guint, GdkModifierType modifiers, gpointer data)
{
    auto *self = static_cast<FileManagerWindow *>(data);
    if (modifiers & GDK_ALT_MASK) {
        if (keyval == GDK_KEY_Left) { self->navigate_history(-1); return TRUE; }
        if (keyval == GDK_KEY_Right) { self->navigate_history(1); return TRUE; }
        if (keyval == GDK_KEY_Up) { on_up_clicked(nullptr, self); return TRUE; }
        if (keyval == GDK_KEY_Home) {
            GFile *home = g_file_new_for_path(g_get_home_dir());
            self->navigate_to(home, true);
            g_object_unref(home);
            return TRUE;
        }
    }
    if (keyval == GDK_KEY_Escape && self->search_entry_ != nullptr &&
        gtk_widget_get_visible(self->search_entry_)) {
        self->hide_search();
        self->update_status();
        return TRUE;
    }
    if ((modifiers & GDK_CONTROL_MASK) && (keyval == GDK_KEY_f || keyval == GDK_KEY_F)) {
        self->show_search();
        return TRUE;
    }
    if (keyval == GDK_KEY_F5 ||
        ((modifiers & GDK_CONTROL_MASK) && (keyval == GDK_KEY_r || keyval == GDK_KEY_R))) {
        self->refresh_current_directory();
        return TRUE;
    }
    if (!(modifiers & GDK_CONTROL_MASK)) return FALSE;
    if ((keyval == GDK_KEY_t || keyval == GDK_KEY_T) &&
        !(modifiers & GDK_SHIFT_MASK)) {
        if (self->current_location_ != nullptr) self->add_tab(self->current_location_);
        return TRUE;
    }
    if (keyval == GDK_KEY_w || keyval == GDK_KEY_W) {
        self->close_tab(self->active_tab_);
        return TRUE;
    }
    if ((keyval == GDK_KEY_Page_Down || keyval == GDK_KEY_Page_Up) &&
        (modifiers & GDK_SHIFT_MASK)) {
        self->move_active_tab(keyval == GDK_KEY_Page_Down ? 1 : -1);
        return TRUE;
    }
    if ((keyval == GDK_KEY_n || keyval == GDK_KEY_N) && !(modifiers & GDK_SHIFT_MASK)) {
        self->open_new_window();
        return TRUE;
    }
    if ((keyval == GDK_KEY_t || keyval == GDK_KEY_T) && (modifiers & GDK_SHIFT_MASK)) {
        self->reopen_closed_tab();
        return TRUE;
    }
    if ((keyval == GDK_KEY_d || keyval == GDK_KEY_D) && (modifiers & GDK_SHIFT_MASK)) {
        self->duplicate_active_tab();
        return TRUE;
    }
    if (keyval == GDK_KEY_1 || keyval == GDK_KEY_2 || keyval == GDK_KEY_3) {
        const auto mode = static_cast<ViewMode>(keyval - GDK_KEY_1);
        self->set_view_mode(mode);
        self->remember_folder_view(mode);
        return TRUE;
    }
    if (keyval == GDK_KEY_h || keyval == GDK_KEY_H) {
        self->toggle_show_hidden();
        return TRUE;
    }
    if (keyval == GDK_KEY_b || keyval == GDK_KEY_B) {
        self->toggle_sidebar();
        return TRUE;
    }
    if (keyval == GDK_KEY_Page_Down || keyval == GDK_KEY_Page_Up) {
        if (self->tabs_.size() <= 1U) return FALSE;
        const std::size_t next = keyval == GDK_KEY_Page_Down
            ? (self->active_tab_ + 1U) % self->tabs_.size()
            : (self->active_tab_ + self->tabs_.size() - 1U) % self->tabs_.size();
        self->activate_tab(next);
        return TRUE;
    }
    return FALSE;
}

void FileManagerWindow::on_back_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    static_cast<FileManagerWindow *>(user_data)->navigate_history(-1);
}

void FileManagerWindow::on_forward_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    static_cast<FileManagerWindow *>(user_data)->navigate_history(1);
}

void FileManagerWindow::on_up_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    auto *self = static_cast<FileManagerWindow *>(user_data);
    if (self->current_location_ == nullptr) return;
    GFile *parent = g_file_get_parent(self->current_location_);
    if (parent != nullptr) {
        char *child_name = g_file_get_basename(self->current_location_);
        self->navigate_to(parent, true);
        self->pending_tab_selection_.clear();
        if (child_name != nullptr) self->pending_tab_selection_.emplace_back(child_name);
        g_free(child_name);
        self->restore_tab_selection();
        g_object_unref(parent);
    }
}

void FileManagerWindow::on_location_activate(GtkEntry *entry, gpointer user_data)
{
    auto *self = static_cast<FileManagerWindow *>(user_data);
    GFile *file = self->file_from_location_text(gtk_editable_get_text(GTK_EDITABLE(entry)));
    if (file != nullptr) {
        self->open_location(file);
        g_object_unref(file);
    }
}

void FileManagerWindow::on_sidebar_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    (void)box;
    auto *self = static_cast<FileManagerWindow *>(user_data);
    const auto *target = static_cast<const char *>(g_object_get_data(G_OBJECT(row), "ifm-target"));
    GFile *file = self->file_from_location_text(target);
    if (file != nullptr) {
        self->navigate_to(file, true);
        g_object_unref(file);
    }
}

void FileManagerWindow::on_list_activate(GtkColumnView *view, const guint position, gpointer user_data)
{
    (void)view;
    static_cast<FileManagerWindow *>(user_data)->activate_position(position);
}

void FileManagerWindow::on_grid_activate(GtkGridView *view, const guint position, gpointer user_data)
{
    (void)view;
    static_cast<FileManagerWindow *>(user_data)->activate_position(position);
}

void FileManagerWindow::on_view_mode_toggled(GtkToggleButton *button, gpointer user_data)
{
    if (!gtk_toggle_button_get_active(button)) {
        return;
    }

    const int encoded = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "ifm-view-mode"));
    if (encoded < 1 || encoded > 3) {
        return;
    }

    auto *self = static_cast<FileManagerWindow *>(user_data);
    if (self->view_mode_programmatic_) return;
    const auto mode = static_cast<ViewMode>(encoded - 1);
    self->set_view_mode(mode);
    self->remember_folder_view(mode);
}


GtkWidget *FileManagerWindow::make_inline_editor(GtkWidget *cell, GtkWidget *label)
{
    GtkWidget *stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_widget_set_hexpand(stack, TRUE);
    gtk_stack_add_named(GTK_STACK(stack), label, "name");
    GtkWidget *entry = gtk_entry_new();
    gtk_widget_set_hexpand(entry, TRUE);
    gtk_widget_set_size_request(entry, 112, -1);
    gtk_stack_add_named(GTK_STACK(stack), entry, "edit");
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "name");
    g_object_set_data(G_OBJECT(cell), "ifm-inline-stack", stack);
    g_object_set_data(G_OBJECT(cell), "ifm-inline-entry", entry);
    g_signal_connect(entry, "activate", G_CALLBACK(on_inline_entry_activate), this);
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_inline_key), this);
    gtk_widget_add_controller(entry, keys);
    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_inline_focus_leave), this);
    gtk_widget_add_controller(entry, focus);
    return stack;
}
void FileManagerWindow::cancel_inline_editor(GtkWidget *cell)
{
    auto *stack = GTK_WIDGET(g_object_get_data(G_OBJECT(cell), "ifm-inline-stack"));
    if (GTK_IS_STACK(stack))
        gtk_stack_set_visible_child_name(GTK_STACK(stack), "name");
}
GtkWidget *FileManagerWindow::find_editor_for_name(GtkWidget *root, const char *name)
{
    if (root == nullptr || name == nullptr) return nullptr;
    const char *stored = static_cast<const char *>(
        g_object_get_data(G_OBJECT(root), "ifm-source-name"));
    if (stored != nullptr && g_strcmp0(stored, name) == 0 &&
        g_object_get_data(G_OBJECT(root), "ifm-inline-entry") != nullptr)
        return root;
    for (GtkWidget *child = gtk_widget_get_first_child(root);
         child != nullptr; child = gtk_widget_get_next_sibling(child)) {
        if (GtkWidget *found = find_editor_for_name(child, name); found != nullptr)
            return found;
    }
    return nullptr;
}
bool FileManagerWindow::begin_inline_rename()
{
    if (primary_selection_ == nullptr || content_stack_ == nullptr) return false;
    auto *info = G_FILE_INFO(gtk_single_selection_get_selected_item(primary_selection_));
    if (info == nullptr) return false;
    const char *name = g_file_info_get_name(info);
    GtkWidget *scroller = gtk_stack_get_visible_child(GTK_STACK(content_stack_));
    if (!GTK_IS_SCROLLED_WINDOW(scroller)) return false;
    GtkWidget *view = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scroller));
    GtkWidget *cell = find_editor_for_name(view, name);
    if (cell == nullptr) return false;
    GtkWidget *stack = GTK_WIDGET(g_object_get_data(G_OBJECT(cell), "ifm-inline-stack"));
    GtkWidget *entry = GTK_WIDGET(g_object_get_data(G_OBJECT(cell), "ifm-inline-entry"));
    if (!GTK_IS_STACK(stack) || !GTK_IS_ENTRY(entry)) return false;
    gtk_editable_set_text(GTK_EDITABLE(entry), name);
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "edit");
    gtk_widget_grab_focus(entry);
    // Preserve the extension while preselecting the base name.
    const char *dot = g_strrstr(name, ".");
    const int length = dot != nullptr && dot != name
        ? static_cast<int>(dot - name) : -1;
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, length);
    return true;
}
void FileManagerWindow::on_inline_entry_activate(GtkEntry *entry, gpointer user_data)
{
    auto *self = static_cast<FileManagerWindow *>(user_data);
    GtkWidget *stack = gtk_widget_get_parent(GTK_WIDGET(entry));
    GtkWidget *cell = stack != nullptr ? gtk_widget_get_parent(stack) : nullptr;
    if (cell == nullptr || self->current_location_ == nullptr) return;
    const char *old_name = static_cast<const char *>(
        g_object_get_data(G_OBJECT(cell), "ifm-source-name"));
    const char *new_name = gtk_editable_get_text(GTK_EDITABLE(entry));
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "name");
    auto *selected = G_FILE_INFO(gtk_single_selection_get_selected_item(self->primary_selection_));
    if (selected == nullptr || old_name == nullptr ||
        g_strcmp0(old_name, g_file_info_get_name(selected)) != 0 ||
        new_name == nullptr || g_strcmp0(new_name, old_name) == 0)
        return;
    auto *operations = static_cast<ItemOperationController *>(
        g_object_get_data(G_OBJECT(self->window_), "ifm-item-controller"));
    if (operations != nullptr) operations->rename_selected_to(new_name);
}
gboolean FileManagerWindow::on_inline_key(GtkEventControllerKey *controller, guint keyval,
                                          guint, GdkModifierType, gpointer user_data)
{
    (void)user_data;
    if (keyval != GDK_KEY_Escape) return FALSE;
    GtkWidget *entry = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
    GtkWidget *stack = entry != nullptr ? gtk_widget_get_parent(entry) : nullptr;
    if (GTK_IS_STACK(stack)) gtk_stack_set_visible_child_name(GTK_STACK(stack), "name");
    return TRUE;
}
void FileManagerWindow::on_inline_focus_leave(GtkEventControllerFocus *focus, gpointer)
{
    GtkWidget *entry = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(focus));
    GtkWidget *stack = entry != nullptr ? gtk_widget_get_parent(entry) : nullptr;
    if (GTK_IS_STACK(stack)) gtk_stack_set_visible_child_name(GTK_STACK(stack), "name");
}

void FileManagerWindow::on_factory_setup(GtkSignalListItemFactory *factory,
                                         GtkListItem *item,
                                         gpointer user_data)
{
    auto *self = static_cast<FileManagerWindow *>(user_data);

    const int encoded = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(factory), "ifm-detail-field"));
    if (encoded < static_cast<int>(DetailField::Name) ||
        encoded > static_cast<int>(DetailField::Modified)) {
        return;
    }

    const auto field = static_cast<DetailField>(encoded);
    if (field == DetailField::Name) {
        GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(cell, "ifm-detail-cell");
        gtk_widget_set_margin_start(cell, 8);
        gtk_widget_set_margin_end(cell, 6);
        gtk_widget_set_margin_top(cell, 2);
        gtk_widget_set_margin_bottom(cell, 2);

        GtkWidget *icon = gtk_image_new();
        gtk_image_set_pixel_size(GTK_IMAGE(icon), kDetailIconSize);
        gtk_widget_set_size_request(icon, kDetailIconSize, -1);
        gtk_box_append(GTK_BOX(cell), icon);

        GtkWidget *name = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(name), 0.0F);
        gtk_label_set_single_line_mode(GTK_LABEL(name), TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_widget_set_halign(name, GTK_ALIGN_FILL);
        gtk_widget_add_css_class(name, "ifm-file-name");
        gtk_box_append(GTK_BOX(cell), self->make_inline_editor(cell, name));

        g_object_set_data(G_OBJECT(cell), "ifm-icon", icon);
        g_object_set_data(G_OBJECT(cell), "ifm-name", name);
        gtk_list_item_set_child(item, cell);
        return;
    }

    GtkWidget *label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label), field == DetailField::Size ? 1.0F : 0.0F);
    gtk_label_set_single_line_mode(GTK_LABEL(label), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(label, 6);
    gtk_widget_set_margin_end(label, 6);
    gtk_widget_set_margin_top(label, 2);
    gtk_widget_set_margin_bottom(label, 2);
    gtk_widget_set_halign(label, GTK_ALIGN_FILL);
    gtk_widget_add_css_class(label, "ifm-detail-secondary");
    gtk_list_item_set_child(item, label);
    if (field == DetailField::Modified) {
        self->modified_cells_.insert(label);
        g_object_weak_ref(
            G_OBJECT(label), &FileManagerWindow::on_modified_cell_finalized, self);
    }
}

void FileManagerWindow::on_factory_bind(GtkSignalListItemFactory *factory,
                                        GtkListItem *item,
                                        gpointer user_data)
{
    auto *self = static_cast<FileManagerWindow *>(user_data);

    auto *info = G_FILE_INFO(gtk_list_item_get_item(item));
    GtkWidget *child = gtk_list_item_get_child(item);
    if (info == nullptr || child == nullptr) {
        return;
    }

    const int encoded = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(factory), "ifm-detail-field"));
    if (encoded < static_cast<int>(DetailField::Name) ||
        encoded > static_cast<int>(DetailField::Modified)) {
        return;
    }

    if (static_cast<DetailField>(encoded) == DetailField::Name) {
        const char *raw = g_file_info_get_name(info);
        g_object_set_data_full(G_OBJECT(child), "ifm-source-name", g_strdup(raw != nullptr ? raw : ""), g_free);
        self->cancel_inline_editor(child);
    }

    switch (static_cast<DetailField>(encoded)) {
    case DetailField::Name: {
        auto *icon = GTK_IMAGE(g_object_get_data(G_OBJECT(child), "ifm-icon"));
        auto *name = GTK_LABEL(g_object_get_data(G_OBJECT(child), "ifm-name"));
        if (GIcon *file_icon = g_file_info_get_icon(info); file_icon != nullptr) {
            gtk_image_set_from_gicon(icon, file_icon);
        } else {
            gtk_image_set_from_icon_name(icon, "text-x-generic-symbolic");
        }
        const char *display_name = g_file_info_get_display_name(info);
        gtk_label_set_text(name, display_name != nullptr ? display_name : "");
        if (self->thumbnails_ != nullptr)
            self->thumbnails_->bind(icon, info,
                gtk_directory_list_get_file(self->directory_list_), kDetailIconSize);
        break;
    }
    case DetailField::Type: {
        const std::string value = detail_type_text(info);
        gtk_label_set_text(GTK_LABEL(child), value.c_str());
        break;
    }
    case DetailField::Size: {
        const std::string value = detail_size_text(info);
        gtk_label_set_text(GTK_LABEL(child), value.c_str());
        break;
    }
    case DetailField::Modified: {
        g_object_set_data_full(
            G_OBJECT(child), "ifm-modified-info", g_object_ref(info),
            reinterpret_cast<GDestroyNotify>(g_object_unref));
        const std::string value = detail_modified_text(info);
        gtk_label_set_text(GTK_LABEL(child), value.c_str());
        break;
    }
    }
}

void FileManagerWindow::on_factory_unbind(GtkSignalListItemFactory *factory,
                                          GtkListItem *item,
                                          gpointer user_data)
{
    (void)user_data;
    const int encoded = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(factory), "ifm-detail-field"));
    if (encoded == static_cast<int>(DetailField::Name)) {
        if (GtkWidget *cell = gtk_list_item_get_child(item); cell != nullptr) {
            static_cast<FileManagerWindow *>(user_data)->cancel_inline_editor(cell);
            g_object_set_data_full(G_OBJECT(cell), "ifm-source-name", nullptr, nullptr);
        }
        return;
    }
    if (encoded != static_cast<int>(DetailField::Modified)) return;
    if (GtkWidget *child = gtk_list_item_get_child(item); child != nullptr) {
        g_object_set_data_full(
            G_OBJECT(child), "ifm-modified-info", nullptr, nullptr);
    }
}

void FileManagerWindow::on_icon_factory_setup(GtkSignalListItemFactory *factory,
                                              GtkListItem *item,
                                              gpointer user_data)
{
    (void)user_data;

    const int icon_size = std::max(48, GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(factory), "ifm-icon-size")));
    const int tile_width = std::max(108, GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(factory), "ifm-tile-width")));

    GtkWidget *tile = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
    gtk_widget_set_size_request(tile, tile_width, -1);
    gtk_widget_set_halign(tile, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(tile, GTK_ALIGN_START);
    gtk_widget_add_css_class(tile, "ifm-icon-tile");

    GtkWidget *icon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(icon), icon_size);
    gtk_widget_set_halign(icon, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(tile), icon);

    GtkWidget *name = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(name), 0.5F);
    gtk_label_set_justify(GTK_LABEL(name), GTK_JUSTIFY_CENTER);
    gtk_label_set_wrap(GTK_LABEL(name), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(name), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines(GTK_LABEL(name), 2);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(name), 18);
    gtk_widget_add_css_class(name, "ifm-file-name");
    gtk_box_append(GTK_BOX(tile), static_cast<FileManagerWindow *>(user_data)->make_inline_editor(tile, name));

    g_object_set_data(G_OBJECT(tile), "ifm-icon", icon);
    g_object_set_data(G_OBJECT(tile), "ifm-name", name);
    gtk_list_item_set_child(item, tile);
}

void FileManagerWindow::on_compact_factory_setup(GtkSignalListItemFactory *factory,
                                                 GtkListItem *item,
                                                 gpointer user_data)
{
    (void)factory;
    (void)user_data;

    // Compact is a distinct dense-scan composition, not a scaled-down Visual tile:
    // the name sits beside a small icon so several readable columns can coexist.
    GtkWidget *tile = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_size_request(tile, 196, -1);
    gtk_widget_set_halign(tile, GTK_ALIGN_FILL);
    gtk_widget_set_valign(tile, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(tile, "ifm-icon-tile");
    gtk_widget_add_css_class(tile, "ifm-compact-tile");

    GtkWidget *icon = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 24);
    gtk_widget_set_halign(icon, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(tile), icon);

    GtkWidget *name = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(name), 0.0F);
    gtk_label_set_single_line_mode(GTK_LABEL(name), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(name, TRUE);
    gtk_widget_set_halign(name, GTK_ALIGN_FILL);
    gtk_widget_add_css_class(name, "ifm-file-name");
    gtk_box_append(GTK_BOX(tile), static_cast<FileManagerWindow *>(user_data)->make_inline_editor(tile, name));

    g_object_set_data(G_OBJECT(tile), "ifm-icon", icon);
    g_object_set_data(G_OBJECT(tile), "ifm-name", name);
    gtk_list_item_set_child(item, tile);
}

void FileManagerWindow::on_icon_factory_bind(GtkSignalListItemFactory *factory,
                                             GtkListItem *item,
                                             gpointer user_data)
{
    (void)factory;

    auto *info = G_FILE_INFO(gtk_list_item_get_item(item));
    GtkWidget *tile = gtk_list_item_get_child(item);
    if (info == nullptr || tile == nullptr) {
        return;
    }

    auto *icon = GTK_IMAGE(g_object_get_data(G_OBJECT(tile), "ifm-icon"));
    auto *name = GTK_LABEL(g_object_get_data(G_OBJECT(tile), "ifm-name"));

    if (GIcon *file_icon = g_file_info_get_icon(info); file_icon != nullptr) {
        gtk_image_set_from_gicon(icon, file_icon);
    } else {
        gtk_image_set_from_icon_name(icon, "text-x-generic-symbolic");
    }

    const char *raw_name = g_file_info_get_name(info);
    g_object_set_data_full(G_OBJECT(tile), "ifm-source-name",
                           g_strdup(raw_name != nullptr ? raw_name : ""), g_free);
    static_cast<FileManagerWindow *>(user_data)->cancel_inline_editor(tile);
    const char *display_name = g_file_info_get_display_name(info);
    gtk_label_set_text(name, display_name != nullptr ? display_name : "");
    if (static_cast<FileManagerWindow *>(user_data)->thumbnails_ != nullptr)
        static_cast<FileManagerWindow *>(user_data)->thumbnails_->bind(
            icon, info,
            gtk_directory_list_get_file(static_cast<FileManagerWindow *>(user_data)->directory_list_),
            gtk_image_get_pixel_size(icon));
}

void FileManagerWindow::on_icon_factory_unbind(GtkSignalListItemFactory *, GtkListItem *item, gpointer user_data)
{
    GtkWidget *cell = gtk_list_item_get_child(item);
    if (cell != nullptr) {
        static_cast<FileManagerWindow *>(user_data)->cancel_inline_editor(cell);
        g_object_set_data_full(G_OBJECT(cell), "ifm-source-name", nullptr, nullptr);
    }
}

void FileManagerWindow::on_loading_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)object;
    (void)pspec;
    static_cast<FileManagerWindow *>(user_data)->update_status();
}

void FileManagerWindow::on_launch_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    (void)source;
    auto *status = GTK_LABEL(user_data);
    GError *error = nullptr;
    if (!g_app_info_launch_default_for_uri_finish(result, &error)) {
        if (error != nullptr) {
            if (gtk_widget_get_root(GTK_WIDGET(status)) != nullptr) {
                gtk_label_set_text(status, error->message);
            }
            g_error_free(error);
        }
    }
    g_object_unref(status);
}

void FileManagerWindow::on_multi_selection_changed(GtkSelectionModel *model,
                                                   const guint position,
                                                   const guint n_items,
                                                   gpointer user_data)
{
    (void)model;
    (void)position;
    (void)n_items;
    auto *self = static_cast<FileManagerWindow *>(user_data);
    self->sync_primary_from_multi();
    if (self->current_location_available_ && self->directory_list_ != nullptr &&
        !gtk_directory_list_is_loading(self->directory_list_) &&
        gtk_directory_list_get_error(self->directory_list_) == nullptr) {
        GListModel *visible = gtk_multi_selection_get_model(self->selection_);
        const guint count = visible != nullptr ? g_list_model_get_n_items(visible) : 0U;
        GtkBitset *selected = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(self->selection_));
        const guint chosen = static_cast<guint>(gtk_bitset_get_size(selected));
        gtk_bitset_unref(selected);
        std::string text = std::to_string(count) + (count == 1U ? " item" : " items");
        if (chosen != 0U) text += "  ·  " + std::to_string(chosen) + " selected";
        gtk_label_set_text(GTK_LABEL(self->status_label_), text.c_str());
    }
}

void FileManagerWindow::on_primary_selection_changed(GObject *object,
                                                     GParamSpec *pspec,
                                                     gpointer user_data)
{
    (void)object;
    (void)pspec;
    static_cast<FileManagerWindow *>(user_data)->sync_multi_from_primary();
}

} // namespace infiltrator::files
