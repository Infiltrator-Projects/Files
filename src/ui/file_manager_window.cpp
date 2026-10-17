// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_manager_window.hpp"
#include "detail_metadata.hpp"
#include "item_operation_controller.hpp"

#include <infiltratr/design.h>
#include <infiltratr/temporal_posix.h>

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string>

namespace infiltrator::files {

namespace {

constexpr const char *kDirectoryAttributes =
    "standard::name,standard::display-name,standard::type,standard::size,"
    "standard::content-type,standard::icon,standard::is-hidden,standard::is-symlink,"
    "time::modified,time::modified-usec,time::modified-nsec";

constexpr const char *kLocationUnavailableStatus =
    "Location unavailable. Reopen the location to retry.";

constexpr int kDetailIconSize = 24;
constexpr int kDetailTypeWidth = 160;
constexpr int kDetailSizeWidth = 96;
constexpr int kDetailModifiedWidth = 190;

enum class DetailField {
    Name = 1,
    Type,
    Size,
    Modified,
};

std::string colour_hex(const std::uint32_t rgb)
{
    char buffer[8]{};
    std::snprintf(buffer, sizeof(buffer), "#%06X", rgb & 0x00FFFFFFU);
    return buffer;
}

GtkWidget *make_sidebar_content(const char *title, const char *icon_name)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 10);
    gtk_widget_set_margin_top(box, 7);
    gtk_widget_set_margin_bottom(box, 7);

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
    return scroll;
}

int compare_detail_items(gconstpointer left, gconstpointer right, gpointer user_data)
{
    auto *left_info = G_FILE_INFO(const_cast<gpointer>(left));
    auto *right_info = G_FILE_INFO(const_cast<gpointer>(right));
    const int encoded = GPOINTER_TO_INT(user_data);
    if (left_info == nullptr || right_info == nullptr ||
        encoded < static_cast<int>(DetailField::Name) ||
        encoded > static_cast<int>(DetailField::Modified)) {
        return 0;
    }

    switch (static_cast<DetailField>(encoded)) {
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
    apply_theme();
    arm_temporal_policy_monitor();
    load_bookmarks();
    tabs_.push_back(TabState{});
    update_tab_buttons();

    g_object_weak_ref(G_OBJECT(window_), &FileManagerWindow::on_window_finalized, this);

    GFile *home = g_file_new_for_path(g_get_home_dir());
    navigate_to(home, true);
    g_object_unref(home);
}

FileManagerWindow::~FileManagerWindow()
{
    // Stop external callbacks before any UI/model references owned by this object are released.
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
    gtk_window_set_default_size(GTK_WINDOW(window_), 1120, 760);

    GtkWidget *header = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(window_), header);

    GtkWidget *nav_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
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
    gtk_widget_add_css_class(view_switcher, "linked");
    gtk_widget_add_css_class(view_switcher, "ifm-view-switcher");

    list_view_button_ = gtk_toggle_button_new_with_label("List");
    icon_view_button_ = gtk_toggle_button_new_with_label("Icons");
    compact_view_button_ = gtk_toggle_button_new_with_label("Compact");
    gtk_widget_set_tooltip_text(list_view_button_, "Detail / list view");
    gtk_widget_set_tooltip_text(icon_view_button_, "Visual / icon view");
    gtk_widget_set_tooltip_text(compact_view_button_, "Compact dense-scan view");

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
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), view_switcher);

    g_signal_connect(back_button_, "clicked", G_CALLBACK(on_back_clicked), this);
    g_signal_connect(forward_button_, "clicked", G_CALLBACK(on_forward_clicked), this);
    g_signal_connect(up_button_, "clicked", G_CALLBACK(on_up_clicked), this);
    g_signal_connect(location_entry_, "activate", G_CALLBACK(on_location_activate), this);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(window_), root);

    GtkWidget *tab_line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(tab_line, "ifm-tab-line");
    gtk_box_append(GTK_BOX(root), tab_line);
    GtkWidget *tab_scroll = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(tab_scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tab_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    tab_strip_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    gtk_widget_set_margin_start(tab_strip_, 6);
    gtk_widget_set_margin_top(tab_strip_, 3);
    gtk_widget_set_margin_bottom(tab_strip_, 3);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tab_scroll), tab_strip_);
    gtk_box_append(GTK_BOX(tab_line), tab_scroll);
    GtkWidget *add_tab_button = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_set_tooltip_text(add_tab_button, "New tab (Ctrl+T)");
    g_signal_connect(add_tab_button, "clicked", G_CALLBACK(on_tab_add_clicked), this);
    gtk_box_append(GTK_BOX(tab_line), add_tab_button);
    bookmark_button_ = gtk_button_new_from_icon_name("starred-symbolic");
    gtk_widget_set_tooltip_text(bookmark_button_, "Pin or unpin current folder");
    g_signal_connect(bookmark_button_, "clicked", G_CALLBACK(on_bookmark_toggle), this);
    gtk_box_append(GTK_BOX(tab_line), bookmark_button_);
    GtkEventController *tab_keys = gtk_event_controller_key_new();
    g_signal_connect(tab_keys, "key-pressed", G_CALLBACK(on_tab_key_pressed), this);
    gtk_widget_add_controller(window_, tab_keys);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_box_append(GTK_BOX(root), paned);

    sidebar_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(sidebar_), GTK_SELECTION_NONE);
    gtk_widget_set_size_request(sidebar_, 220, -1);
    gtk_widget_add_css_class(sidebar_, "ifm-sidebar");

    GtkWidget *sidebar_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sidebar_scroll), sidebar_);
    gtk_paned_set_start_child(GTK_PANED(paned), sidebar_scroll);
    gtk_paned_set_position(GTK_PANED(paned), 220);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);

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
    mounted_places_monitor_ = std::make_unique<MountedPlacesMonitor>([this]() {
        refresh_mounted_places();
    });
    refresh_mounted_places();
    g_signal_connect(sidebar_, "row-activated", G_CALLBACK(on_sidebar_row_activated), this);

    directory_list_ = gtk_directory_list_new(kDirectoryAttributes, nullptr);
    gtk_directory_list_set_monitored(directory_list_, TRUE);
    g_signal_connect(directory_list_, "notify::loading", G_CALLBACK(on_loading_changed), this);
    g_signal_connect(directory_list_, "notify::error", G_CALLBACK(on_loading_changed), this);

    GtkSortListModel *sort_model =
        gtk_sort_list_model_new(G_LIST_MODEL(g_object_ref(directory_list_)), nullptr);
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
    gtk_column_view_set_single_click_activate(GTK_COLUMN_VIEW(list), FALSE);
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
        gtk_column_view_column_set_resizable(column, FALSE);
        if (fixed_width > 0) {
            gtk_column_view_column_set_fixed_width(column, fixed_width);
        }

        GtkSorter *sorter = GTK_SORTER(gtk_custom_sorter_new(
            compare_detail_items,
            GINT_TO_POINTER(static_cast<int>(field)),
            nullptr));
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
    gtk_grid_view_set_max_columns(GTK_GRID_VIEW(compact), 64U);
    gtk_widget_add_css_class(compact, "ifm-icon-grid");
    gtk_widget_add_css_class(compact, "ifm-compact-grid");
    g_signal_connect(compact, "activate", G_CALLBACK(on_grid_activate), this);

    content_stack_ = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(content_stack_), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_stack_add_named(GTK_STACK(content_stack_), make_scroller(list), "list");
    gtk_stack_add_named(GTK_STACK(content_stack_), make_scroller(icons), "icons");
    gtk_stack_add_named(GTK_STACK(content_stack_), make_scroller(compact), "compact");
    gtk_paned_set_end_child(GTK_PANED(paned), content_stack_);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(list_view_button_), TRUE);

    GtkWidget *status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(status, 12);
    gtk_widget_set_margin_end(status, 12);
    gtk_widget_set_margin_top(status, 5);
    gtk_widget_set_margin_bottom(status, 5);
    gtk_widget_add_css_class(status, "ifm-status");

    spinner_ = gtk_spinner_new();
    gtk_widget_set_visible(spinner_, FALSE);
    gtk_box_append(GTK_BOX(status), spinner_);

    status_label_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(status_label_), 0.0F);
    gtk_widget_set_hexpand(status_label_, TRUE);
    gtk_box_append(GTK_BOX(status), status_label_);
    gtk_box_append(GTK_BOX(root), status);
}

void FileManagerWindow::apply_theme()
{
    GtkSettings *settings = gtk_settings_get_default();
    gboolean system_dark = FALSE;
    if (settings != nullptr) {
        g_object_get(settings, "gtk-application-prefer-dark-theme", &system_dark, nullptr);
    }

    const InfiltratrThemePalette *palette =
        infiltratr_theme_resolve(INFILTRATR_THEME_SYSTEM, system_dark != FALSE);
    const InfiltratrDesignMetrics *metrics = infiltratr_design_metrics();
    const InfiltratrTypography *typography = infiltratr_typography();
    if (palette == nullptr || metrics == nullptr || typography == nullptr) {
        return;
    }

    std::ostringstream css;
    css << "window { background: " << colour_hex(palette->background_rgb)
        << "; color: " << colour_hex(palette->text_rgb)
        << "; font-family: '" << typography->ui_family << "', " << typography->gtk_fallback << "; }\n"
        << "headerbar { background: " << colour_hex(palette->titlebar_rgb)
        << "; color: " << colour_hex(palette->title_rgb)
        << "; border-bottom: 1px solid " << colour_hex(palette->border_rgb) << "; }\n"
        << ".ifm-sidebar { background: " << colour_hex(palette->panel_rgb)
        << "; border-right: 1px solid " << colour_hex(palette->border_rgb) << "; }\n"
        << ".ifm-sidebar row { margin: 2px 6px; border-radius: " << metrics->control_radius << "px; }\n"
        << ".ifm-sidebar row:hover { background: " << colour_hex(palette->surface_hover_rgb) << "; }\n"
        << ".ifm-file-list, .ifm-icon-grid { background: " << colour_hex(palette->background_rgb) << "; }\n"
        << ".ifm-file-list row { border-radius: " << metrics->control_radius << "px; }\n"
        << ".ifm-file-list row:hover { background: " << colour_hex(palette->surface_hover_rgb) << "; }\n"
        << ".ifm-file-list row:selected { background: " << colour_hex(palette->selection_background_rgb)
        << "; color: " << colour_hex(palette->selection_foreground_rgb) << "; }\n"
        << ".ifm-file-list header, .ifm-file-list header button { background: "
        << colour_hex(palette->panel_rgb) << "; color: " << colour_hex(palette->muted_rgb)
        << "; border-color: " << colour_hex(palette->border_rgb) << "; }\n"
        << ".ifm-icon-tile { margin: 4px; padding: 8px 6px; border-radius: "
        << metrics->control_radius << "px; }\n"
        << ".ifm-icon-tile:hover { background: " << colour_hex(palette->surface_hover_rgb) << "; }\n"
        << ".ifm-compact-tile { margin: 2px; padding: 5px 8px; }\n"
        << ".ifm-file-name { color: " << colour_hex(palette->text_rgb) << "; }\n"
        << ".ifm-detail-secondary { color: " << colour_hex(palette->muted_rgb) << "; }\n"
        << ".ifm-file-list row:selected .ifm-file-name, "
        << ".ifm-file-list row:selected .ifm-detail-secondary { color: "
        << colour_hex(palette->selection_foreground_rgb) << "; }\n"
        << ".ifm-status { background: " << colour_hex(palette->panel_rgb)
        << "; color: " << colour_hex(palette->muted_rgb)
        << "; border-top: 1px solid " << colour_hex(palette->border_rgb) << "; }\n"
        << "entry { background: " << colour_hex(palette->input_rgb)
        << "; color: " << colour_hex(palette->text_rgb)
        << "; border-radius: " << metrics->control_radius << "px; }\n";

    GtkCssProvider *provider = gtk_css_provider_new();
    const std::string stylesheet = css.str();
    gtk_css_provider_load_from_data(provider, stylesheet.c_str(), static_cast<gssize>(stylesheet.size()));
    gtk_style_context_add_provider_for_display(gtk_widget_get_display(window_),
                                               GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
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
    gtk_directory_list_set_file(directory_list_, file);
    gtk_editable_set_text(GTK_EDITABLE(location_entry_), parse_name);

    const std::string title = location.display_name.empty() ? "Files" : "Files — " + location.display_name;
    gtk_window_set_title(GTK_WINDOW(window_), title.c_str());

    g_free(uri_text);
    g_free(parse_name);
    update_navigation_state();
    update_status();
    update_tab_buttons();
    update_bookmark_button();
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

void FileManagerWindow::set_view_mode(const ViewMode mode)
{
    if (content_stack_ == nullptr) {
        return;
    }

    view_mode_ = mode;
    GtkWidget *active_button = mode == ViewMode::List ? list_view_button_ :
        mode == ViewMode::Icons ? icon_view_button_ : compact_view_button_;
    if (GTK_IS_TOGGLE_BUTTON(active_button) &&
        !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(active_button))) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(active_button), TRUE);
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
        gtk_spinner_stop(GTK_SPINNER(spinner_));
        gtk_widget_set_visible(spinner_, FALSE);
        gtk_label_set_text(GTK_LABEL(status_label_), kLocationUnavailableStatus);
        return;
    }

    if (gtk_directory_list_is_loading(directory_list_)) {
        gtk_widget_set_visible(spinner_, TRUE);
        gtk_spinner_start(GTK_SPINNER(spinner_));
        gtk_label_set_text(GTK_LABEL(status_label_), "Loading…");
        return;
    }

    gtk_spinner_stop(GTK_SPINNER(spinner_));
    gtk_widget_set_visible(spinner_, FALSE);

    if (const GError *error = gtk_directory_list_get_error(directory_list_); error != nullptr) {
        gtk_label_set_text(GTK_LABEL(status_label_), error->message);
        return;
    }

    restore_tab_selection();
    const guint count = g_list_model_get_n_items(G_LIST_MODEL(directory_list_));
    const std::string text = std::to_string(count) + (count == 1U ? " item" : " items");
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
    if (file == nullptr) return;
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
    const TabState &tab = tabs_[index];
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
    if (tabs_.size() < 2U || index >= tabs_.size()) return;
    const bool current = index == active_tab_;
    if (current) {
        // Do not overwrite the tab being removed with an in-flight view snapshot.
        tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));
        active_tab_ = tabs_.size();
        const std::size_t next = std::min(index, tabs_.size() - 1U);
        active_tab_ = next;
        const TabState tab = tabs_[next];
        history_ = tab.history;
        history_index_ = tab.index;
        pending_tab_selection_ = tab.selected_names;
        if (!tab.uri.empty()) {
            GFile *file = g_file_new_for_uri(tab.uri.c_str());
            navigate_to(file, false);
            g_object_unref(file);
        }
        set_view_mode(tab.view);
        restore_tab_selection();
    } else {
        tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));
        if (index < active_tab_) --active_tab_;
    }
    update_tab_buttons();
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
        gtk_widget_add_css_class(group, "linked");
        GtkWidget *button = gtk_button_new();
        gtk_widget_set_size_request(button, 92, -1);
        gtk_widget_set_hexpand(button, FALSE);
        gtk_widget_add_css_class(button, index == active_tab_ ? "suggested-action" : "flat");
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
        gtk_box_append(GTK_BOX(group), button);
        GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
        gtk_widget_set_tooltip_text(close, "Close tab (Ctrl+W)");
        gtk_widget_add_css_class(close, "flat");
        gtk_widget_set_sensitive(close, tabs_.size() > 1U);
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
    if (!(modifiers & GDK_CONTROL_MASK)) return FALSE;
    auto *self = static_cast<FileManagerWindow *>(data);
    if (keyval == GDK_KEY_t || keyval == GDK_KEY_T) {
        if (self->current_location_ != nullptr) self->add_tab(self->current_location_);
        return TRUE;
    }
    if (keyval == GDK_KEY_w || keyval == GDK_KEY_W) {
        if (self->tabs_.size() <= 1U) return FALSE;
        self->close_tab(self->active_tab_);
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

void FileManagerWindow::load_bookmarks()
{
    char *path = g_build_filename(g_get_user_config_dir(), "infiltrator", "files",
                                  "bookmarks", nullptr);
    gchar *contents = nullptr;
    gsize bytes = 0;
    if (g_file_get_contents(path, &contents, &bytes, nullptr) && contents != nullptr) {
        gchar **lines = g_strsplit(contents, "\n", 129);
        for (gsize i = 0; lines[i] != nullptr && bookmarks_.size() < 128U; ++i) {
            std::string uri(lines[i]);
            char *scheme = g_uri_parse_scheme(uri.c_str());
            const bool valid = scheme != nullptr;
            g_free(scheme);
            if (uri.empty() || !valid) continue;
            if (std::none_of(bookmarks_.begin(), bookmarks_.end(),
                             [&](const BookmarkRow &row){ return row.uri == uri; })) {
                append_bookmark(uri);
            }
        }
        g_strfreev(lines);
    }
    g_free(contents);
    g_free(path);
}
void FileManagerWindow::save_bookmarks() const
{
    char *folder = g_build_filename(g_get_user_config_dir(), "infiltrator", "files", nullptr);
    if (g_mkdir_with_parents(folder, 0700) == 0) {
        char *path = g_build_filename(folder, "bookmarks", nullptr);
        std::string data;
        for (const BookmarkRow &row : bookmarks_) data += row.uri + "\n";
        g_file_set_contents(path, data.c_str(), static_cast<gssize>(data.size()), nullptr);
        g_free(path);
    }
    g_free(folder);
}
void FileManagerWindow::append_bookmark(const std::string &uri)
{
    GFile *file = g_file_new_for_uri(uri.c_str());
    char *label = g_file_get_basename(file);
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *symbol = gtk_image_new_from_icon_name("starred-symbolic");
    gtk_box_append(GTK_BOX(content), symbol);
    GtkWidget *name = gtk_label_new(label != nullptr ? label : uri.c_str());
    gtk_label_set_xalign(GTK_LABEL(name), 0.0F);
    gtk_widget_set_hexpand(name, TRUE);
    gtk_box_append(GTK_BOX(content), name);
    GtkWidget *options = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(options), "view-more-symbolic");
    gtk_widget_set_tooltip_text(options, "Bookmark options");
    GtkWidget *popup = gtk_popover_new();
    GtkWidget *items = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    for (const char *verb : {"Move up", "Move down", "Remove"}) {
        GtkWidget *action = gtk_button_new_with_label(verb);
        gtk_widget_add_css_class(action, "flat");
        g_object_set_data(G_OBJECT(action), "ifm-bookmark-row", row);
        g_signal_connect(action, "clicked", G_CALLBACK(on_bookmark_action), this);
        gtk_box_append(GTK_BOX(items), action);
    }
    gtk_popover_set_child(GTK_POPOVER(popup), items);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(options), popup);
    gtk_box_append(GTK_BOX(content), options);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), content);
    g_object_set_data_full(G_OBJECT(row), "ifm-target", g_strdup(uri.c_str()), g_free);
    gtk_list_box_append(GTK_LIST_BOX(sidebar_), row);
    bookmarks_.push_back(BookmarkRow{uri, row});
    g_free(label);
    g_object_unref(file);
}
void FileManagerWindow::reorder_bookmarks()
{
    for (std::size_t i = 0; i < bookmarks_.size(); ++i) {
        GtkWidget *row = bookmarks_[i].row;
        const int desired = static_cast<int>(static_sidebar_row_count_ +
                                             mounted_place_rows_.size() + i);
        if (gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(row)) != desired) {
            g_object_ref(row);
            gtk_list_box_remove(GTK_LIST_BOX(sidebar_), row);
            gtk_list_box_insert(GTK_LIST_BOX(sidebar_), row, desired);
            g_object_unref(row);
        }
    }
}
void FileManagerWindow::remove_bookmark(const std::size_t index)
{
    if (index >= bookmarks_.size()) return;
    GtkWidget *row = bookmarks_[index].row;
    g_object_ref(row);
    gtk_list_box_remove(GTK_LIST_BOX(sidebar_), row);
    bookmarks_.erase(bookmarks_.begin() + static_cast<std::ptrdiff_t>(index));
    g_object_unref(row);
    reorder_bookmarks();
    save_bookmarks();
    update_bookmark_button();
}
void FileManagerWindow::update_bookmark_button()
{
    if (bookmark_button_ == nullptr || current_location_ == nullptr) return;
    char *uri = g_file_get_uri(current_location_);
    const bool pinned = uri != nullptr &&
        std::any_of(bookmarks_.begin(), bookmarks_.end(),
                    [uri](const BookmarkRow &bookmark){ return bookmark.uri == uri; });
    gtk_widget_set_tooltip_text(bookmark_button_,
                                pinned ? "Unpin current folder" : "Pin current folder");
    g_free(uri);
}
void FileManagerWindow::on_bookmark_toggle(GtkButton *, gpointer data)
{
    auto *self = static_cast<FileManagerWindow *>(data);
    if (self->current_location_ == nullptr) return;
    char *uri = g_file_get_uri(self->current_location_);
    if (uri == nullptr) return;
    const auto it = std::find_if(self->bookmarks_.begin(), self->bookmarks_.end(),
                                [uri](const BookmarkRow &entry){ return entry.uri == uri; });
    if (it != self->bookmarks_.end()) {
        self->remove_bookmark(static_cast<std::size_t>(it - self->bookmarks_.begin()));
    } else if (self->bookmarks_.size() < 128U) {
        self->append_bookmark(uri);
        self->save_bookmarks();
        self->update_bookmark_button();
    }
    g_free(uri);
}
void FileManagerWindow::on_bookmark_action(GtkButton *button, gpointer data)
{
    auto *self = static_cast<FileManagerWindow *>(data);
    GtkWidget *row = GTK_WIDGET(g_object_get_data(G_OBJECT(button), "ifm-bookmark-row"));
    const auto it = std::find_if(self->bookmarks_.begin(), self->bookmarks_.end(),
                               [row](const BookmarkRow &entry){ return entry.row == row; });
    if (it == self->bookmarks_.end()) return;
    const std::size_t index = static_cast<std::size_t>(it - self->bookmarks_.begin());
    const char *verb = gtk_button_get_label(button);
    if (g_strcmp0(verb, "Remove") == 0) {
        self->remove_bookmark(index);
    } else if (g_strcmp0(verb, "Move up") == 0 && index > 0U) {
        std::swap(self->bookmarks_[index], self->bookmarks_[index - 1U]);
        self->reorder_bookmarks();
        self->save_bookmarks();
    } else if (g_strcmp0(verb, "Move down") == 0 &&
               index + 1U < self->bookmarks_.size()) {
        std::swap(self->bookmarks_[index], self->bookmarks_[index + 1U]);
        self->reorder_bookmarks();
        self->save_bookmarks();
    }
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
    if (self->current_location_ == nullptr) {
        return;
    }
    GFile *parent = g_file_get_parent(self->current_location_);
    if (parent != nullptr) {
        self->navigate_to(parent, true);
        g_object_unref(parent);
    }
}

void FileManagerWindow::on_location_activate(GtkEntry *entry, gpointer user_data)
{
    auto *self = static_cast<FileManagerWindow *>(user_data);
    GFile *file = self->file_from_location_text(gtk_editable_get_text(GTK_EDITABLE(entry)));
    if (file != nullptr) {
        self->navigate_to(file, true);
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
    self->set_view_mode(static_cast<ViewMode>(encoded - 1));
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
        gtk_widget_set_margin_start(cell, 8);
        gtk_widget_set_margin_end(cell, 6);
        gtk_widget_set_margin_top(cell, 5);
        gtk_widget_set_margin_bottom(cell, 5);

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
    gtk_widget_set_margin_top(label, 5);
    gtk_widget_set_margin_bottom(label, 5);
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
    static_cast<FileManagerWindow *>(user_data)->sync_primary_from_multi();
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
