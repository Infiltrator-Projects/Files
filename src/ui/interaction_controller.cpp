// SPDX-License-Identifier: GPL-3.0-or-later
#include "interaction_controller.hpp"

#include "detail_metadata.hpp"
#include "file_inspection.hpp"
#include "file_manager_window.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace infiltrator::files {

namespace {

bool tooltip_equals(GtkWidget *widget, const char *expected)
{
    if (widget == nullptr || expected == nullptr) {
        return false;
    }
    const char *tooltip = gtk_widget_get_tooltip_text(widget);
    return g_strcmp0(tooltip, expected) == 0;
}

GtkWidget *find_widget_by_tooltip(GtkWidget *root, const char *tooltip)
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
        if (GtkWidget *match = find_widget_by_tooltip(child, tooltip); match != nullptr) {
            return match;
        }
    }
    return nullptr;
}

GtkWidget *find_button_by_label(GtkWidget *root, const char *label)
{
    if (root == nullptr || label == nullptr) {
        return nullptr;
    }
    if (GTK_IS_BUTTON(root) &&
        g_strcmp0(gtk_button_get_label(GTK_BUTTON(root)), label) == 0) {
        return root;
    }
    for (GtkWidget *child = gtk_widget_get_first_child(root);
         child != nullptr;
         child = gtk_widget_get_next_sibling(child)) {
        if (GtkWidget *match = find_button_by_label(child, label); match != nullptr) {
            return match;
        }
    }
    return nullptr;
}

GtkWidget *find_descendant_with_name_label(GtkWidget *root)
{
    if (root == nullptr) {
        return nullptr;
    }
    if (g_object_get_data(G_OBJECT(root), "ifm-name") != nullptr) {
        return root;
    }
    for (GtkWidget *child = gtk_widget_get_first_child(root);
         child != nullptr;
         child = gtk_widget_get_next_sibling(child)) {
        if (GtkWidget *match = find_descendant_with_name_label(child); match != nullptr) {
            return match;
        }
    }
    return nullptr;
}

GtkWidget *menu_action(GtkWidget *menu_button, const char *label)
{
    if (!GTK_IS_MENU_BUTTON(menu_button)) {
        return nullptr;
    }
    GtkWidget *popover = GTK_WIDGET(
        gtk_menu_button_get_popover(GTK_MENU_BUTTON(menu_button)));
    return find_button_by_label(popover, label);
}

void add_separator(GtkWidget *box)
{
    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(separator, 3);
    gtk_widget_set_margin_bottom(separator, 3);
    gtk_box_append(GTK_BOX(box), separator);
}

std::string file_display_name(GFile *file)
{
    if (file == nullptr) {
        return {};
    }
    char *basename = g_file_get_basename(file);
    if (basename != nullptr && basename[0] != '\0') {
        std::string result(basename);
        g_free(basename);
        return result;
    }
    g_free(basename);

    char *parse_name = g_file_get_parse_name(file);
    std::string result = parse_name != nullptr ? parse_name : "";
    g_free(parse_name);
    return result;
}

void add_property_row(GtkGrid *grid, int row, const char *name, const std::string &value)
{
    GtkWidget *name_label = gtk_label_new(name);
    gtk_label_set_xalign(GTK_LABEL(name_label), 0.0F);
    gtk_widget_add_css_class(name_label, "dim-label");
    gtk_widget_set_valign(name_label, GTK_ALIGN_START);

    GtkWidget *value_label = gtk_label_new(value.c_str());
    gtk_label_set_xalign(GTK_LABEL(value_label), 0.0F);
    gtk_label_set_selectable(GTK_LABEL(value_label), TRUE);
    gtk_label_set_wrap(GTK_LABEL(value_label), TRUE);
    gtk_widget_set_hexpand(value_label, TRUE);
    gtk_widget_set_halign(value_label, GTK_ALIGN_FILL);

    gtk_grid_attach(grid, name_label, 0, row, 1, 1);
    gtk_grid_attach(grid, value_label, 1, row, 1, 1);
}

} // namespace

InteractionController::InteractionController(GtkWindow *window,
                                             GtkDirectoryList *directory_list,
                                             GtkMultiSelection *selection,
                                             GtkSingleSelection *primary_selection,
                                             GtkWidget *location_entry,
                                             GtkWidget *content_stack,
                                             GtkWidget *status_label)
    : window_(window),
      directory_list_(directory_list),
      selection_(selection),
      primary_selection_(primary_selection),
      location_entry_(location_entry),
      content_stack_(content_stack),
      status_label_(status_label)
{
    if (window_ == nullptr || directory_list_ == nullptr || selection_ == nullptr ||
        primary_selection_ == nullptr || location_entry_ == nullptr ||
        content_stack_ == nullptr) {
        return;
    }

    g_object_ref(directory_list_);
    g_object_ref(selection_);
    g_object_ref(primary_selection_);

    discover_existing_actions();
    install_breadcrumb_surface();
    install_action_surfaces();
    install_context_gestures();
    install_keyboard_shortcuts();

    selection_handler_ = g_signal_connect(
        selection_, "selection-changed",
        G_CALLBACK(&InteractionController::on_selection_changed), this);
    location_handler_ = g_signal_connect(
        directory_list_, "notify::file",
        G_CALLBACK(&InteractionController::on_location_changed), this);

    rebuild_breadcrumbs();
    update_action_state();
    g_object_weak_ref(G_OBJECT(window_), &InteractionController::on_window_finalized, this);
}

InteractionController::~InteractionController()
{
    if (selection_ != nullptr) {
        if (selection_handler_ != 0U) {
            g_signal_handler_disconnect(selection_, selection_handler_);
        }
        g_object_unref(selection_);
        selection_ = nullptr;
    }
    if (directory_list_ != nullptr) {
        if (location_handler_ != 0U) {
            g_signal_handler_disconnect(directory_list_, location_handler_);
        }
        g_object_unref(directory_list_);
        directory_list_ = nullptr;
    }
    if (primary_selection_ != nullptr) {
        g_object_unref(primary_selection_);
        primary_selection_ = nullptr;
    }
    if (context_popover_ != nullptr) {
        if (gtk_widget_get_parent(context_popover_) != nullptr) {
            gtk_widget_unparent(context_popover_);
        }
        g_object_unref(context_popover_);
        context_popover_ = nullptr;
    }
}

void InteractionController::on_window_finalized(gpointer user_data, GObject *where_object_was)
{
    (void)where_object_was;
    auto *self = static_cast<InteractionController *>(user_data);
    self->window_ = nullptr;
    delete self;
}

void InteractionController::discover_existing_actions()
{
    GtkWidget *titlebar = gtk_window_get_titlebar(window_);
    if (!GTK_IS_HEADER_BAR(titlebar)) {
        return;
    }

    GtkWidget *new_folder_button =
        find_widget_by_tooltip(titlebar, "New Folder (Ctrl+Shift+N)");
    GtkWidget *new_file_button =
        find_widget_by_tooltip(titlebar, "New Empty File (Ctrl+Alt+N)");
    GtkWidget *single_menu =
        find_widget_by_tooltip(titlebar, "Selected item actions");
    GtkWidget *batch_menu =
        find_widget_by_tooltip(titlebar, "Actions for multiple selected items");
    GtkWidget *destructive_menu =
        find_widget_by_tooltip(titlebar, "Trash, restore and permanent delete");

    new_folder_target_ = new_folder_button;
    new_file_target_ = new_file_button;
    rename_target_ = menu_action(single_menu, "Rename");
    copy_target_ = menu_action(single_menu, "Copy to…");
    move_target_ = menu_action(single_menu, "Move to…");
    batch_copy_target_ = menu_action(batch_menu, "Copy selected to…");
    batch_move_target_ = menu_action(batch_menu, "Move selected to…");
    trash_target_ = menu_action(destructive_menu, "Move to Trash");
    restore_target_ = menu_action(destructive_menu, "Restore");
    delete_target_ = menu_action(destructive_menu, "Delete Permanently…");

    for (GtkWidget *legacy : {new_folder_button, new_file_button, single_menu, batch_menu, destructive_menu}) {
        if (legacy != nullptr) {
            gtk_widget_set_visible(legacy, FALSE);
        }
    }
}

void InteractionController::install_breadcrumb_surface()
{
    GtkWidget *titlebar = gtk_window_get_titlebar(window_);
    if (!GTK_IS_HEADER_BAR(titlebar)) {
        return;
    }

    g_object_ref(location_entry_);

    title_root_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_hexpand(title_root_, TRUE);
    gtk_widget_set_size_request(title_root_, 220, -1);

    title_stack_ = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(title_stack_), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_widget_set_hexpand(title_stack_, TRUE);
    gtk_box_append(GTK_BOX(title_root_), title_stack_);

    breadcrumb_box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_hexpand(breadcrumb_box_, TRUE);
    gtk_widget_set_halign(breadcrumb_box_, GTK_ALIGN_FILL);
    gtk_stack_add_named(GTK_STACK(title_stack_), breadcrumb_box_, "breadcrumbs");

    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(titlebar), title_root_);
    gtk_stack_add_named(GTK_STACK(title_stack_), location_entry_, "entry");
    g_object_unref(location_entry_);

    edit_location_button_ = gtk_button_new_from_icon_name("document-edit-symbolic");
    gtk_widget_set_tooltip_text(edit_location_button_, "Edit location (Ctrl+L)");
    gtk_widget_add_css_class(edit_location_button_, "flat");
    gtk_box_append(GTK_BOX(title_root_), edit_location_button_);
    g_signal_connect(edit_location_button_, "clicked",
                     G_CALLBACK(&InteractionController::on_edit_location_clicked), this);

    show_breadcrumbs();
}

GtkWidget *InteractionController::create_action_button(
    GtkWidget *box,
    const char *label,
    GtkWidget *target,
    const ActionScope scope,
    std::vector<ProxyAction> &proxies,
    const GCallback callback)
{
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_set_halign(button, GTK_ALIGN_FILL);
    gtk_box_append(GTK_BOX(box), button);

    if (callback != nullptr) {
        g_signal_connect(button, "clicked", callback, this);
    } else {
        g_object_set_data(G_OBJECT(button), "ifm-proxy-target", target);
        g_signal_connect(button, "clicked",
                         G_CALLBACK(&InteractionController::on_proxy_clicked), this);
    }

    proxies.push_back(ProxyAction{button, target, scope, callback == nullptr});
    return button;
}

GtkWidget *InteractionController::add_command(GtkWidget *box, const char *label,
                                              const char *command, ActionScope scope,
                                              std::vector<ProxyAction> &proxies)
{
    GtkWidget *button = create_action_button(box, label, nullptr, scope, proxies,
                                             G_CALLBACK(&InteractionController::on_workspace_clicked));
    g_object_set_data(G_OBJECT(button), "ifm-workspace-command", const_cast<char *>(command));
    return button;
}

GtkWidget *InteractionController::build_action_box(std::vector<ProxyAction> &proxies,
                                                   GtkWidget **open_button,
                                                   GtkWidget **properties_button)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);

    *open_button = create_action_button(
        box, "Open", nullptr, ActionScope::SingleSelection, proxies,
        G_CALLBACK(&InteractionController::on_open_clicked));
    GtkWidget *open_with = add_command(box, "Open With…", "open-with",
                                       ActionScope::SingleSelection, proxies);
    open_with_actions_.push_back(open_with);

    GtkWidget *open_tab = gtk_button_new_with_label("Open folder in new tab");
    gtk_widget_add_css_class(open_tab, "flat");
    gtk_box_append(GTK_BOX(box), open_tab);
    open_in_tab_actions_.push_back(open_tab);
    g_signal_connect(open_tab, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
        auto *self = static_cast<InteractionController *>(data);
        if (self->window_ != nullptr) {
            auto *window = static_cast<FileManagerWindow *>(
                g_object_get_data(G_OBJECT(self->window_), "ifm-window-controller"));
            if (window != nullptr) (void)window->open_primary_directory_in_new_tab();
        }
        self->close_popovers();
    }), this);
    GtkWidget *open_window = add_command(box, "Open folder in new window", "folder-window",
                                        ActionScope::SingleSelection, proxies);
    open_in_window_actions_.push_back(open_window);
    if (&proxies == &overflow_proxies_) {
        add_command(box, "New Window", "new-window", ActionScope::Always, proxies);
        add_command(box, "Duplicate Tab", "duplicate-tab", ActionScope::Always, proxies);
        add_command(box, "Move Tab to New Window", "detach-tab", ActionScope::Always, proxies);
        add_command(box, "Reopen Closed Tab", "reopen-tab", ActionScope::Always, proxies);
    }
    create_action_button(box, "New Folder", new_folder_target_,
                         ActionScope::Always, proxies);
    create_action_button(box, "New Empty File", new_file_target_,
                         ActionScope::Always, proxies);
    add_separator(box);

    create_action_button(box, "Rename", rename_target_,
                         ActionScope::SingleSelection, proxies);
    create_action_button(box, "Copy to…", copy_target_,
                         ActionScope::SingleSelection, proxies);
    create_action_button(box, "Move to…", move_target_,
                         ActionScope::SingleSelection, proxies);

    create_action_button(box, "Copy selected to…", batch_copy_target_,
                         ActionScope::MultiSelection, proxies);
    create_action_button(box, "Move selected to…", batch_move_target_,
                         ActionScope::MultiSelection, proxies);
    add_command(box, "Open selected folders in tabs", "selected-tabs",
                ActionScope::MultiSelection, proxies);

    add_separator(box);
    create_action_button(box, "Move to Trash", trash_target_,
                         ActionScope::AnySelection, proxies);
    create_action_button(box, "Restore", restore_target_,
                         ActionScope::SingleSelection, proxies);
    create_action_button(box, "Delete Permanently…", delete_target_,
                         ActionScope::AnySelection, proxies);

    add_separator(box);
    add_command(box, "Copy Path as Text", "copy-path", ActionScope::SingleSelection, proxies);
    add_command(box, "Copy URI as Text", "copy-uri", ActionScope::SingleSelection, proxies);
    add_command(box, "Copy selected paths as text", "copy-selected-paths",
                ActionScope::MultiSelection, proxies);
    add_command(box, "Copy selected URIs as text", "copy-selected-uris",
                ActionScope::MultiSelection, proxies);
    add_separator(box);
    add_command(box, "Select by filename pattern…", "select-pattern",
                ActionScope::Always, proxies);
    add_command(box, "Select same file type", "select-type",
                ActionScope::SingleSelection, proxies);
    *properties_button = create_action_button(
        box, "Properties", nullptr, ActionScope::SingleSelection, proxies,
        G_CALLBACK(&InteractionController::on_properties_clicked));

    if (&proxies == &overflow_proxies_) {
        add_separator(box);
        GtkWidget *tools_expander = gtk_expander_new("Inspection and tools");
        gtk_box_append(GTK_BOX(box), tools_expander);
        GtkWidget *tools = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_expander_set_child(GTK_EXPANDER(tools_expander), tools);
        add_command(tools, "Open terminal here", "terminal-current",
                    ActionScope::Always, proxies);
        folder_terminal_actions_.push_back(add_command(
            tools, "Open selected folder in terminal", "terminal-selected",
            ActionScope::SingleSelection, proxies));
        add_command(tools, "Inspect file…", "inspect",
                    ActionScope::SingleSelection, proxies);
        compare_actions_.push_back(add_command(
            tools, "Compare two file hashes…", "compare-hashes",
            ActionScope::MultiSelection, proxies));
        add_separator(box);
        GtkWidget *expander = gtk_expander_new("View and layout");
        gtk_widget_add_css_class(expander, "ifm-layout-section");
        gtk_box_append(GTK_BOX(box), expander);
        GtkWidget *layout = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_expander_set_child(GTK_EXPANDER(expander), layout);
        hidden_actions_.push_back(add_command(layout, "Show Hidden Files", "toggle-hidden",
                                              ActionScope::Always, proxies));
        folder_sort_actions_.push_back(add_command(layout, "Folders First", "folders-first",
                                                   ActionScope::Always, proxies));
        sidebar_actions_.push_back(add_command(layout, "Hide Sidebar", "toggle-sidebar",
                                               ActionScope::Always, proxies));
    }
    return box;
}

// Context actions represent what the pointer actually addresses. Unlike the
// complete toolbar catalogue, an empty area is not a selected file, and a
// regular item is not a Trash recovery target. These sections are reconciled
// on every selection/location change rather than duplicating operation logic.
GtkWidget *InteractionController::build_context_action_box()
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box, "ifm-context-actions");
    gtk_widget_set_margin_start(box, 5);
    gtk_widget_set_margin_end(box, 5);
    gtk_widget_set_margin_top(box, 5);
    gtk_widget_set_margin_bottom(box, 5);

    const auto section = [&](bool separated) {
        GtkWidget *group = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        if (separated) add_separator(group);
        gtk_box_append(GTK_BOX(box), group);
        context_sections_.push_back(group);
        return group;
    };

    GtkWidget *opening = section(false);
    context_open_button_ = create_action_button(
        opening, "Open", nullptr, ActionScope::SingleSelection, context_proxies_,
        G_CALLBACK(&InteractionController::on_open_clicked));
    open_with_actions_.push_back(add_command(
        opening, "Open With…", "open-with", ActionScope::SingleSelection,
        context_proxies_));

    GtkWidget *new_tab = gtk_button_new_with_label("Open folder in new tab");
    gtk_widget_add_css_class(new_tab, "flat");
    gtk_box_append(GTK_BOX(opening), new_tab);
    open_in_tab_actions_.push_back(new_tab);
    g_signal_connect(new_tab, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
        auto *self = static_cast<InteractionController *>(data);
        if (self->window_ != nullptr) {
            auto *window = static_cast<FileManagerWindow *>(
                g_object_get_data(G_OBJECT(self->window_), "ifm-window-controller"));
            if (window != nullptr) (void)window->open_primary_directory_in_new_tab();
        }
        self->close_popovers();
    }), this);
    open_in_window_actions_.push_back(add_command(
        opening, "Open folder in new window", "folder-window",
        ActionScope::SingleSelection, context_proxies_));

    GtkWidget *creation = section(true);
    create_action_button(creation, "New Folder", new_folder_target_,
                         ActionScope::NoSelection, context_proxies_);
    create_action_button(creation, "New Empty File", new_file_target_,
                         ActionScope::NoSelection, context_proxies_);

    GtkWidget *clipboard = section(true);
    for (const char *name : {"Cut", "Copy", "Paste"}) {
        GtkWidget *button = create_action_button(
            clipboard, name, nullptr,
            g_strcmp0(name, "Paste") == 0 ? ActionScope::Always
                                          : ActionScope::AnySelection,
            context_proxies_);
        // ClipboardController installs its existing validated handlers after
        // this controller. Resolve the target lazily, never fork the operation.
        g_object_set_data(G_OBJECT(button), "ifm-clipboard-action",
                          const_cast<char *>(name));
    }

    GtkWidget *transfer = section(true);
    create_action_button(transfer, "Rename", rename_target_,
                         ActionScope::SingleSelection, context_proxies_);
    create_action_button(transfer, "Copy to…", copy_target_,
                         ActionScope::SingleSelection, context_proxies_);
    create_action_button(transfer, "Move to…", move_target_,
                         ActionScope::SingleSelection, context_proxies_);
    create_action_button(transfer, "Copy selected to…", batch_copy_target_,
                         ActionScope::MultiSelection, context_proxies_);
    create_action_button(transfer, "Move selected to…", batch_move_target_,
                         ActionScope::MultiSelection, context_proxies_);

    GtkWidget *removal = section(true);
    create_action_button(removal, "Move to Trash", trash_target_,
                         ActionScope::AnySelection, context_proxies_);
    create_action_button(removal, "Restore", restore_target_,
                         ActionScope::SingleSelection, context_proxies_);
    create_action_button(removal, "Delete Permanently…", delete_target_,
                         ActionScope::AnySelection, context_proxies_);

    GtkWidget *details = section(true);
    context_properties_button_ = create_action_button(
        details, "Properties", nullptr, ActionScope::SingleSelection,
        context_proxies_, G_CALLBACK(&InteractionController::on_properties_clicked));
    add_command(details, "Inspect file…", "inspect",
                ActionScope::SingleSelection, context_proxies_);
    folder_terminal_actions_.push_back(add_command(
        details, "Open folder in terminal", "terminal-selected",
        ActionScope::SingleSelection, context_proxies_));
    compare_actions_.push_back(add_command(
        details, "Compare two file hashes…", "compare-hashes",
        ActionScope::MultiSelection, context_proxies_));
    add_command(details, "Open terminal here", "terminal-current",
                ActionScope::NoSelection, context_proxies_);
    return box;
}

void InteractionController::install_action_surfaces()
{
    GtkWidget *titlebar = gtk_window_get_titlebar(window_);
    if (!GTK_IS_HEADER_BAR(titlebar)) {
        return;
    }

    overflow_menu_button_ = gtk_menu_button_new();
    gtk_menu_button_set_child(
        GTK_MENU_BUTTON(overflow_menu_button_),
        gtk_image_new_from_icon_name("open-menu-symbolic"));
    gtk_widget_set_tooltip_text(overflow_menu_button_, "File actions");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(titlebar), overflow_menu_button_);

    overflow_popover_ = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(overflow_popover_), TRUE);

    // The action catalogue is taller than a laptop screen. Never make the
    // native popover grow to the full unbounded height of every action:
    // constrain its scrolled main section, leaving file clipboard controls
    // permanently available at the bottom of the menu.
    GtkWidget *overflow_root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(overflow_root, "ifm-overflow-menu");
    GtkWidget *overflow_scroller = gtk_scrolled_window_new();
    gtk_widget_add_css_class(overflow_scroller, "ifm-overflow-scroll");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(overflow_scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(overflow_scroller), TRUE);
    gtk_scrolled_window_set_max_content_height(
        GTK_SCROLLED_WINDOW(overflow_scroller), 340);
    gtk_widget_set_size_request(overflow_scroller, 270, -1);
    GtkWidget *overflow_box =
        build_action_box(overflow_proxies_,
                         &overflow_open_button_,
                         &overflow_properties_button_);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(overflow_scroller), overflow_box);
    gtk_box_append(GTK_BOX(overflow_root), overflow_scroller);
    gtk_popover_set_child(GTK_POPOVER(overflow_popover_), overflow_root);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(overflow_menu_button_), overflow_popover_);

    context_popover_ = gtk_popover_new();
    g_object_ref_sink(context_popover_);
    gtk_popover_set_has_arrow(GTK_POPOVER(context_popover_), TRUE);
    gtk_widget_add_css_class(context_popover_, "ifm-context-popover");
    GtkWidget *context_scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(context_scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(context_scroller), TRUE);
    gtk_scrolled_window_set_max_content_height(
        GTK_SCROLLED_WINDOW(context_scroller), 330);
    gtk_widget_set_size_request(context_scroller, 220, -1);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(context_scroller),
                                  build_context_action_box());
    gtk_popover_set_child(GTK_POPOVER(context_popover_), context_scroller);
    // Unowned test/inspection handle; the interaction controller and popover
    // remain governed by the existing native window lifetime.
    g_object_set_data(G_OBJECT(window_), "ifm-context-popover", context_popover_);
}

void InteractionController::install_context_gestures()
{
    for (const char *name : {"list", "icons", "compact"}) {
        GtkWidget *scroller = gtk_stack_get_child_by_name(GTK_STACK(content_stack_), name);
        if (!GTK_IS_SCROLLED_WINDOW(scroller)) {
            continue;
        }
        GtkWidget *view = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scroller));
        if (view == nullptr) {
            continue;
        }

        GtkGesture *gesture = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), GDK_BUTTON_SECONDARY);
        g_signal_connect(gesture, "pressed",
                         G_CALLBACK(&InteractionController::on_context_pressed), this);
        gtk_widget_add_controller(view, GTK_EVENT_CONTROLLER(gesture));
    }
}

void InteractionController::install_keyboard_shortcuts()
{
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed",
                     G_CALLBACK(&InteractionController::on_key_pressed), this);
    gtk_widget_add_controller(GTK_WIDGET(window_), keys);
}

void InteractionController::on_proxy_clicked(GtkButton *button, gpointer user_data)
{
    auto *self = static_cast<InteractionController *>(user_data);
    auto *target = GTK_WIDGET(
        g_object_get_data(G_OBJECT(button), "ifm-proxy-target"));
    if (target != nullptr && gtk_widget_get_sensitive(target)) {
        g_signal_emit_by_name(target, "clicked");
    }
    self->close_popovers();
}


void InteractionController::on_workspace_clicked(GtkButton *button, gpointer user_data)
{
    auto *self = static_cast<InteractionController *>(user_data);
    const char *command = static_cast<const char *>(
        g_object_get_data(G_OBJECT(button), "ifm-workspace-command"));
    if (command == nullptr || self->window_ == nullptr) return;
    auto *window = static_cast<FileManagerWindow *>(
        g_object_get_data(G_OBJECT(self->window_), "ifm-window-controller"));
    if (window == nullptr) return;
    if (g_strcmp0(command, "open-with") == 0) self->show_open_with();
    else if (g_strcmp0(command, "new-window") == 0) window->open_new_window();
    else if (g_strcmp0(command, "folder-window") == 0) {
        (void)window->open_primary_directory_in_new_window();
    }
    else if (g_strcmp0(command, "duplicate-tab") == 0) window->duplicate_active_tab();
    else if (g_strcmp0(command, "detach-tab") == 0) window->move_active_tab_to_new_window();
    else if (g_strcmp0(command, "reopen-tab") == 0) window->reopen_closed_tab();
    else if (g_strcmp0(command, "toggle-hidden") == 0) window->toggle_show_hidden();
    else if (g_strcmp0(command, "folders-first") == 0) window->toggle_folders_first();
    else if (g_strcmp0(command, "toggle-sidebar") == 0) window->toggle_sidebar();
    else if (g_strcmp0(command, "selected-tabs") == 0)
        (void)window->open_selected_folders_in_tabs();
    else if (g_strcmp0(command, "select-pattern") == 0)
        self->show_selection_pattern_dialog();
    else if (g_strcmp0(command, "select-type") == 0)
        self->select_same_type();
    else if (g_strcmp0(command, "inspect") == 0)
        self->inspect_primary();
    else if (g_strcmp0(command, "compare-hashes") == 0)
        self->compare_selected_hashes();
    else if (g_strcmp0(command, "terminal-current") == 0)
        self->launch_terminal(false);
    else if (g_strcmp0(command, "terminal-selected") == 0)
        self->launch_terminal(true);
    else if (g_strcmp0(command, "copy-selected-paths") == 0 ||
             g_strcmp0(command, "copy-selected-uris") == 0)
        self->copy_selected_text(g_strcmp0(command, "copy-selected-uris") == 0);
    else if (g_strcmp0(command, "copy-path") == 0 ||
             g_strcmp0(command, "copy-uri") == 0) {
        auto *info = G_FILE_INFO(gtk_single_selection_get_selected_item(self->primary_selection_));
        GFile *parent = gtk_directory_list_get_file(self->directory_list_);
        if (info != nullptr && parent != nullptr) {
            const char *name = g_file_info_get_name(info);
            if (name != nullptr && *name != '\0') {
                GFile *child = g_file_get_child(parent, name);
                const bool uri = g_strcmp0(command, "copy-uri") == 0;
                char *value = uri ? g_file_get_uri(child) : g_file_get_path(child);
                if (value != nullptr) {
                    GdkClipboard *clipboard = gdk_display_get_clipboard(
                        gtk_widget_get_display(GTK_WIDGET(self->window_)));
                    if (clipboard != nullptr) gdk_clipboard_set_text(clipboard, value);
                    if (self->status_label_ != nullptr)
                        gtk_label_set_text(GTK_LABEL(self->status_label_),
                                           uri ? "URI copied" : "Path copied");
                }
                g_free(value);
                g_object_unref(child);
            }
        }
    }
    self->update_action_state();
    self->close_popovers();
}


void InteractionController::show_selection_pattern_dialog()
{
    if (window_ == nullptr || selection_ == nullptr) return;
    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Select by filename pattern");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), window_);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 380, -1);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_window_set_child(GTK_WINDOW(dialog), box);
    gtk_box_append(GTK_BOX(box), gtk_label_new("Select visible filenames matching * and ?"));
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(entry), 256);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "*.pdf");
    gtk_box_append(GTK_BOX(box), entry);
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(box), actions);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    GtkWidget *apply = gtk_button_new_with_label("Select matches");
    gtk_box_append(GTK_BOX(actions), cancel);
    gtk_box_append(GTK_BOX(actions), apply);
    g_object_set_data(G_OBJECT(dialog), "ifm-pattern-entry", entry);
    g_object_set_data(G_OBJECT(dialog), "ifm-pattern-controller", this);
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy), dialog);
    g_signal_connect(apply, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
        auto *win = GTK_WINDOW(data);
        auto *self = static_cast<InteractionController *>(
            g_object_get_data(G_OBJECT(win), "ifm-pattern-controller"));
        auto *input = GTK_EDITABLE(g_object_get_data(G_OBJECT(win), "ifm-pattern-entry"));
        if (self == nullptr || input == nullptr || self->selection_ == nullptr) return;
        const char *pattern = gtk_editable_get_text(input);
        if (pattern == nullptr || *pattern == '\0' ||
            !g_utf8_validate(pattern, -1, nullptr)) return;
        GListModel *model = gtk_multi_selection_get_model(self->selection_);
        if (model == nullptr) return;
        GtkBitset *mask = gtk_bitset_new_empty();
        GtkBitset *matches = gtk_bitset_new_empty();
        const guint count = g_list_model_get_n_items(model);
        for (guint i = 0U; i < count; ++i) {
            gtk_bitset_add(mask, i);
            auto *info = G_FILE_INFO(g_list_model_get_item(model, i));
            if (info != nullptr) {
                const char *name = g_file_info_get_name(info);
                if (name != nullptr && g_pattern_match_simple(pattern, name))
                    gtk_bitset_add(matches, i);
                g_object_unref(info);
            }
        }
        const guint selected = static_cast<guint>(gtk_bitset_get_size(matches));
        gtk_selection_model_set_selection(GTK_SELECTION_MODEL(self->selection_), matches, mask);
        gtk_bitset_unref(mask);
        gtk_bitset_unref(matches);
        if (self->status_label_ != nullptr) {
            const std::string status = std::to_string(selected) + " filenames selected";
            gtk_label_set_text(GTK_LABEL(self->status_label_), status.c_str());
        }
        gtk_window_destroy(win);
    }), dialog);
    gtk_window_present(GTK_WINDOW(dialog));
}

void InteractionController::select_same_type()
{
    if (selection_ == nullptr || primary_selection_ == nullptr) return;
    auto *source = G_FILE_INFO(gtk_single_selection_get_selected_item(primary_selection_));
    if (source == nullptr) return;
    const GFileType kind = g_file_info_get_file_type(source);
    const char *type = g_file_info_get_content_type(source);
    const std::string match_type = type != nullptr ? type : "";
    GListModel *model = gtk_multi_selection_get_model(selection_);
    if (model == nullptr) return;
    GtkBitset *mask = gtk_bitset_new_empty();
    GtkBitset *matches = gtk_bitset_new_empty();
    const guint count = g_list_model_get_n_items(model);
    for (guint i = 0U; i < count; ++i) {
        gtk_bitset_add(mask, i);
        auto *info = G_FILE_INFO(g_list_model_get_item(model, i));
        if (info != nullptr) {
            const char *candidate = g_file_info_get_content_type(info);
            if (g_file_info_get_file_type(info) == kind &&
                (kind == G_FILE_TYPE_DIRECTORY || (kind == G_FILE_TYPE_REGULAR &&
                  !match_type.empty() && g_strcmp0(candidate, match_type.c_str()) == 0)))
                gtk_bitset_add(matches, i);
            g_object_unref(info);
        }
    }
    gtk_selection_model_set_selection(GTK_SELECTION_MODEL(selection_), matches, mask);
    gtk_bitset_unref(mask);
    gtk_bitset_unref(matches);
}

void InteractionController::copy_selected_text(const bool uris)
{
    if (selection_ == nullptr || directory_list_ == nullptr || window_ == nullptr) return;
    GFile *parent = gtk_directory_list_get_file(directory_list_);
    GListModel *model = gtk_multi_selection_get_model(selection_);
    if (parent == nullptr || model == nullptr) return;
    GtkBitset *bits = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(selection_));
    const guint total = static_cast<guint>(gtk_bitset_get_size(bits));
    if (total == 0U || total > 4096U) {
        gtk_bitset_unref(bits);
        if (status_label_ != nullptr)
            gtk_label_set_text(GTK_LABEL(status_label_), "Select up to 4096 items to copy as text.");
        return;
    }
    std::string output;
    GtkBitsetIter iter;
    guint index = 0U;
    bool complete = true;
    for (gboolean valid = gtk_bitset_iter_init_first(&iter, bits, &index);
         valid; valid = gtk_bitset_iter_next(&iter, &index)) {
        auto *info = G_FILE_INFO(g_list_model_get_item(model, index));
        const char *name = info != nullptr ? g_file_info_get_name(info) : nullptr;
        if (name == nullptr || *name == '\0') {
            complete = false;
        } else {
            GFile *item = g_file_get_child(parent, name);
            char *value = uris ? g_file_get_uri(item) : g_file_get_path(item);
            if (value == nullptr || output.size() + strlen(value) + 1U > 1048576U) {
                complete = false;
            } else {
                output += value;
                output += '\n';
            }
            g_free(value);
            g_object_unref(item);
        }
        if (info != nullptr) g_object_unref(info);
        if (!complete) break;
    }
    gtk_bitset_unref(bits);
    if (!complete) {
        if (status_label_ != nullptr) gtk_label_set_text(GTK_LABEL(status_label_),
            uris ? "Selected URIs exceed the text limit." :
                   "Selection includes non-native paths or exceeds the text limit.");
        return;
    }
    GdkClipboard *clipboard = gdk_display_get_clipboard(
        gtk_widget_get_display(GTK_WIDGET(window_)));
    if (clipboard != nullptr) gdk_clipboard_set_text(clipboard, output.c_str());
    if (status_label_ != nullptr) gtk_label_set_text(GTK_LABEL(status_label_),
        uris ? "Selected URIs copied as text" : "Selected native paths copied as text");
}


void InteractionController::show_open_with()
{
    if (window_ == nullptr || directory_list_ == nullptr || primary_selection_ == nullptr)
        return;
    auto *info = G_FILE_INFO(gtk_single_selection_get_selected_item(primary_selection_));
    GFile *parent = gtk_directory_list_get_file(directory_list_);
    if (info == nullptr || parent == nullptr ||
        g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR)
        return;
    const char *name = g_file_info_get_name(info);
    if (name == nullptr || *name == '\0') return;

    const char *type = g_file_info_get_content_type(info);
    char *guessed = nullptr;
    if (type == nullptr || *type == '\0') {
        guessed = g_content_type_guess(name, nullptr, 0, nullptr);
        type = guessed;
    }
    GList *apps = type != nullptr ? g_app_info_get_all_for_type(type) : nullptr;
    if (apps == nullptr) {
        gtk_label_set_text(GTK_LABEL(status_label_),
                           "No compatible installed application is registered for this file type.");
        g_free(guessed);
        return;
    }

    struct Choice {
        GList *apps{nullptr};
        GFile *file{nullptr};
        GtkDropDown *drop{nullptr};
        GtkCheckButton *default_box{nullptr};
        std::string type;
        ~Choice() {
            if (apps) g_list_free_full(apps, g_object_unref);
            if (file) g_object_unref(file);
        }
    };
    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Open With");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), window_);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 440, -1);
    auto *choice = new Choice{apps, g_file_get_child(parent, name), nullptr, nullptr,
                              type != nullptr ? type : ""};
    g_object_set_data_full(G_OBJECT(dialog), "ifm-open-with",
                           choice, [](gpointer data) { delete static_cast<Choice *>(data); });
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_window_set_child(GTK_WINDOW(dialog), box);
    gtk_box_append(GTK_BOX(box), gtk_label_new("Choose an installed application"));
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    for (GList *iter = apps; iter != nullptr; iter = iter->next) {
        auto *app = G_APP_INFO(iter->data);
        g_ptr_array_add(names, g_strdup(g_app_info_get_display_name(app)));
    }
    g_ptr_array_add(names, nullptr);
    GtkWidget *dropdown = gtk_drop_down_new_from_strings(
        reinterpret_cast<const char *const *>(names->pdata));
    g_ptr_array_free(names, TRUE);
    choice->drop = GTK_DROP_DOWN(dropdown);
    gtk_widget_set_hexpand(dropdown, TRUE);
    gtk_box_append(GTK_BOX(box), dropdown);
    GtkWidget *defaults = gtk_check_button_new_with_label(
        "Use this application as the default for this file type");
    choice->default_box = GTK_CHECK_BUTTON(defaults);
    gtk_box_append(GTK_BOX(box), defaults);
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(box), actions);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    GtkWidget *launch = gtk_button_new_with_label("Open");
    gtk_widget_add_css_class(launch, "suggested-action");
    gtk_box_append(GTK_BOX(actions), cancel);
    gtk_box_append(GTK_BOX(actions), launch);
    g_signal_connect_swapped(cancel, "clicked",
                              G_CALLBACK(gtk_window_destroy), dialog);
    g_signal_connect(launch, "clicked", G_CALLBACK((+[](GtkButton *, gpointer raw) {
        auto *win = GTK_WINDOW(raw);
        auto *data = static_cast<Choice *>(g_object_get_data(G_OBJECT(win), "ifm-open-with"));
        if (data == nullptr) return;
        const guint index = gtk_drop_down_get_selected(data->drop);
        if (index == GTK_INVALID_LIST_POSITION) return;
        auto *app = G_APP_INFO(g_list_nth_data(data->apps, index));
        if (app == nullptr) return;
        GList items{data->file, nullptr, nullptr};
        GError *error = nullptr;
        if (!g_app_info_launch(app, &items, nullptr, &error)) {
            GtkAlertDialog *alert = gtk_alert_dialog_new("Could not open file");
            gtk_alert_dialog_set_detail(alert, error != nullptr ? error->message : "Application launch failed.");
            gtk_alert_dialog_show(alert, win);
            g_object_unref(alert);
            g_clear_error(&error);
            return;
        }
        if (gtk_check_button_get_active(data->default_box) &&
            !data->type.empty() &&
            !g_app_info_set_as_default_for_type(app, data->type.c_str(), &error)) {
            GtkAlertDialog *alert = gtk_alert_dialog_new("Default application could not be saved");
            gtk_alert_dialog_set_detail(alert, error != nullptr ? error->message : "Association update failed.");
            gtk_alert_dialog_show(alert, win);
            g_object_unref(alert);
            g_clear_error(&error);
            return;
        }
        gtk_window_destroy(win);
    })), dialog);
    gtk_window_present(GTK_WINDOW(dialog));
    g_free(guessed);
}

void InteractionController::on_open_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    auto *self = static_cast<InteractionController *>(user_data);
    self->close_popovers();
    self->open_primary();
}

void InteractionController::on_properties_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    auto *self = static_cast<InteractionController *>(user_data);
    self->close_popovers();
    self->show_properties();
}

void InteractionController::on_edit_location_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    auto *self = static_cast<InteractionController *>(user_data);
    if (self->title_stack_ != nullptr &&
        g_strcmp0(
            gtk_stack_get_visible_child_name(GTK_STACK(self->title_stack_)),
            "entry") == 0) {
        self->show_breadcrumbs();
    } else {
        self->show_location_entry();
    }
}

void InteractionController::on_breadcrumb_clicked(GtkButton *button, gpointer user_data)
{
    auto *self = static_cast<InteractionController *>(user_data);
    const auto *target = static_cast<const char *>(
        g_object_get_data(G_OBJECT(button), "ifm-breadcrumb-target"));
    if (target == nullptr || target[0] == '\0') {
        return;
    }
    gtk_editable_set_text(GTK_EDITABLE(self->location_entry_), target);
    g_signal_emit_by_name(self->location_entry_, "activate");
    self->show_breadcrumbs();
}

void InteractionController::on_selection_changed(GtkSelectionModel *model,
                                                 const guint position,
                                                 const guint n_items,
                                                 gpointer user_data)
{
    (void)model;
    (void)position;
    (void)n_items;
    static_cast<InteractionController *>(user_data)->update_action_state();
}

void InteractionController::on_location_changed(GObject *object,
                                                GParamSpec *pspec,
                                                gpointer user_data)
{
    (void)object;
    (void)pspec;
    auto *self = static_cast<InteractionController *>(user_data);
    self->rebuild_breadcrumbs();
    self->update_action_state();
}

gboolean InteractionController::on_key_pressed(GtkEventControllerKey *controller,
                                               const guint keyval,
                                               const guint keycode,
                                               const GdkModifierType state,
                                               gpointer user_data)
{
    (void)controller;
    (void)keycode;
    auto *self = static_cast<InteractionController *>(user_data);

    const bool control = (state & GDK_CONTROL_MASK) != 0;
    const bool shift = (state & GDK_SHIFT_MASK) != 0;

    if (control && (keyval == GDK_KEY_l || keyval == GDK_KEY_L)) {
        self->show_location_entry();
        return TRUE;
    }
    if (keyval == GDK_KEY_Escape && self->title_stack_ != nullptr &&
        g_strcmp0(
            gtk_stack_get_visible_child_name(GTK_STACK(self->title_stack_)),
            "entry") == 0) {
        self->show_breadcrumbs();
        return TRUE;
    }

    GtkWidget *focus = gtk_window_get_focus(self->window_);
    if (GTK_IS_EDITABLE(focus)) {
        return FALSE;
    }

    if (control && shift && (keyval == GDK_KEY_c || keyval == GDK_KEY_C)) {
        // Text path copy must not replace the normal Ctrl+C file-reference action.
        GtkWidget *command = nullptr;
        for (const ProxyAction &action : self->overflow_proxies_) {
            if (g_strcmp0(static_cast<const char *>(
                    g_object_get_data(G_OBJECT(action.button), "ifm-workspace-command")),
                "copy-path") == 0) { command = action.button; break; }
        }
        if (command != nullptr && gtk_widget_get_sensitive(command))
            on_workspace_clicked(GTK_BUTTON(command), self);
        return TRUE;
    }
    if (control && shift && (keyval == GDK_KEY_a || keyval == GDK_KEY_A)) {
        gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(self->selection_));
        return TRUE;
    }
    if (control && (keyval == GDK_KEY_a || keyval == GDK_KEY_A)) {
        gtk_selection_model_select_all(GTK_SELECTION_MODEL(self->selection_));
        return TRUE;
    }
    if (control && (keyval == GDK_KEY_i || keyval == GDK_KEY_I)) {
        GListModel *model = gtk_multi_selection_get_model(self->selection_);
        const guint count = model != nullptr ? g_list_model_get_n_items(model) : 0U;
        for (guint position = 0U; position < count; ++position) {
            if (gtk_selection_model_is_selected(GTK_SELECTION_MODEL(self->selection_),
                                                position)) {
                gtk_selection_model_unselect_item(GTK_SELECTION_MODEL(self->selection_),
                                                  position);
            } else {
                gtk_selection_model_select_item(GTK_SELECTION_MODEL(self->selection_),
                                                position, FALSE);
            }
        }
        return TRUE;
    }
    return FALSE;
}

void InteractionController::on_context_pressed(GtkGestureClick *gesture,
                                               const int n_press,
                                               const double x,
                                               const double y,
                                               gpointer user_data)
{
    (void)n_press;
    auto *self = static_cast<InteractionController *>(user_data);
    GtkWidget *view =
        gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    if (view == nullptr || self->context_popover_ == nullptr) {
        return;
    }

    self->select_item_at_pointer(view, x, y);
    self->update_action_state();

    if (GtkWidget *parent = gtk_widget_get_parent(self->context_popover_);
        parent != view) {
        if (parent != nullptr) {
            gtk_widget_unparent(self->context_popover_);
        }
        gtk_widget_set_parent(self->context_popover_, view);
    }

    GdkRectangle rectangle{
        static_cast<int>(x),
        static_cast<int>(y),
        1,
        1,
    };
    gtk_popover_set_pointing_to(GTK_POPOVER(self->context_popover_), &rectangle);
    gtk_popover_popup(GTK_POPOVER(self->context_popover_));
}

void InteractionController::on_launch_finished(GObject *source,
                                               GAsyncResult *result,
                                               gpointer user_data)
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

guint InteractionController::selection_count() const
{
    GtkBitset *selected =
        gtk_selection_model_get_selection(GTK_SELECTION_MODEL(selection_));
    const guint count = static_cast<guint>(gtk_bitset_get_size(selected));
    gtk_bitset_unref(selected);
    return count;
}

void InteractionController::update_action_state()
{
    const guint count = selection_count();
    GFile *location = gtk_directory_list_get_file(directory_list_);
    const bool in_trash = location != nullptr &&
        g_file_has_uri_scheme(location, "trash");

    // The clipboard actions are installed last by ClipboardController. Late
    // binding to those buttons retains its safety checks and journalling.
    for (ProxyAction &proxy : context_proxies_) {
        const char *clipboard = static_cast<const char *>(
            g_object_get_data(G_OBJECT(proxy.button), "ifm-clipboard-action"));
        if (clipboard != nullptr) {
            proxy.target = find_button_by_label(overflow_popover_, clipboard);
            g_object_set_data(G_OBJECT(proxy.button), "ifm-proxy-target",
                              proxy.target);
        }
    }

    const auto update = [count, in_trash, this](std::vector<ProxyAction> &proxies) {
        for (ProxyAction &proxy : proxies) {
            bool visible = false;
            switch (proxy.scope) {
            case ActionScope::Always:
                visible = true;
                break;
            case ActionScope::NoSelection:
                visible = count == 0U;
                break;
            case ActionScope::SingleSelection:
                visible = count == 1U;
                break;
            case ActionScope::MultiSelection:
                visible = count > 1U;
                break;
            case ActionScope::AnySelection:
                visible = count > 0U;
                break;
            }

            // These are mutually exclusive locations, not merely disabled
            // alternatives. No recovery command should appear in /sbin.
            if (proxy.target == restore_target_) visible = visible && in_trash;
            if (proxy.target == trash_target_) visible = visible && !in_trash;
            gtk_widget_set_visible(proxy.button, visible);
            if (proxy.requires_target) {
                gtk_widget_set_sensitive(
                    proxy.button,
                    visible && proxy.target != nullptr &&
                    gtk_widget_get_sensitive(proxy.target));
            } else {
                gtk_widget_set_sensitive(proxy.button, visible);
            }
        }
    };

    update(overflow_proxies_);
    update(context_proxies_);
    auto *info = G_FILE_INFO(gtk_single_selection_get_selected_item(primary_selection_));
    const bool folder = count == 1U && info != nullptr &&
        g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    for (GtkWidget *button : open_in_tab_actions_) {
        gtk_widget_set_visible(button, folder);
        gtk_widget_set_sensitive(button, folder);
    }
    for (GtkWidget *button : open_in_window_actions_) {
        gtk_widget_set_visible(button, folder);
        gtk_widget_set_sensitive(button, folder);
    }
    const bool regular = count == 1U && info != nullptr &&
        g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR;
    for (GtkWidget *button : open_with_actions_) {
        gtk_widget_set_visible(button, regular);
        gtk_widget_set_sensitive(button, regular);
    }
    GFile *folder_location = gtk_directory_list_get_file(directory_list_);
    const bool native_location = folder_location != nullptr &&
        g_file_is_native(folder_location);
    bool selected_folder_native = folder && native_location;
    if (selected_folder_native && info != nullptr) {
        GFile *child = g_file_get_child(folder_location, g_file_info_get_name(info));
        selected_folder_native = g_file_is_native(child);
        g_object_unref(child);
    }
    for (GtkWidget *button : folder_terminal_actions_) {
        gtk_widget_set_visible(button, selected_folder_native);
        gtk_widget_set_sensitive(button, selected_folder_native);
    }
    for (ProxyAction &proxy : overflow_proxies_) {
        if (g_strcmp0(static_cast<const char *>(
                g_object_get_data(G_OBJECT(proxy.button), "ifm-workspace-command")),
                "terminal-current") == 0)
            gtk_widget_set_sensitive(proxy.button, native_location);
    }
    for (ProxyAction &proxy : context_proxies_) {
        if (g_strcmp0(static_cast<const char *>(
                g_object_get_data(G_OBJECT(proxy.button), "ifm-workspace-command")),
                "terminal-current") == 0)
            gtk_widget_set_sensitive(proxy.button, native_location);
    }
    bool compare_two_regular = count == 2U;
    if (compare_two_regular) {
        GListModel *model = gtk_multi_selection_get_model(selection_);
        GtkBitset *bits = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(selection_));
        GtkBitsetIter iterator;
        guint position = 0;
        gboolean valid = gtk_bitset_iter_init_first(&iterator, bits, &position);
        while (valid) {
            auto *selected = G_FILE_INFO(g_list_model_get_item(model, position));
            if (selected == nullptr ||
                g_file_info_get_file_type(selected) != G_FILE_TYPE_REGULAR)
                compare_two_regular = false;
            if (selected != nullptr) g_object_unref(selected);
            valid = gtk_bitset_iter_next(&iterator, &position);
        }
        gtk_bitset_unref(bits);
    }
    for (GtkWidget *button : compare_actions_) {
        gtk_widget_set_visible(button, compare_two_regular);
        gtk_widget_set_sensitive(button, compare_two_regular);
    }
    // Suppress empty groups and leading dividers in both selected-file and
    // background menus. Scrolling is only for genuinely crowded contexts.
    bool preceding_section = false;
    for (GtkWidget *section : context_sections_) {
        bool has_action = false;
        GtkWidget *divider = nullptr;
        for (GtkWidget *child = gtk_widget_get_first_child(section);
             child != nullptr; child = gtk_widget_get_next_sibling(child)) {
            if (GTK_IS_SEPARATOR(child)) divider = child;
            else if (gtk_widget_get_visible(child)) has_action = true;
        }
        gtk_widget_set_visible(section, has_action);
        if (divider != nullptr)
            gtk_widget_set_visible(divider, has_action && preceding_section);
        if (has_action) preceding_section = true;
    }
    auto *window = static_cast<FileManagerWindow *>(
        g_object_get_data(G_OBJECT(window_), "ifm-window-controller"));
    if (window != nullptr) {
        for (GtkWidget *button : hidden_actions_)
            gtk_button_set_label(GTK_BUTTON(button), window->show_hidden()
                                     ? "Hide Hidden Files" : "Show Hidden Files");
        for (GtkWidget *button : folder_sort_actions_)
            gtk_button_set_label(GTK_BUTTON(button), window->folders_first()
                                     ? "Folders First ✓" : "Folders First");
        for (GtkWidget *button : sidebar_actions_)
            gtk_button_set_label(GTK_BUTTON(button), window->sidebar_visible()
                                     ? "Hide Sidebar" : "Show Sidebar");
    }

    if (overflow_open_button_ != nullptr) {
        gtk_widget_set_sensitive(overflow_open_button_, count == 1U);
    }
    if (overflow_properties_button_ != nullptr) {
        gtk_widget_set_sensitive(overflow_properties_button_, count == 1U);
    }
    if (context_open_button_ != nullptr) {
        gtk_widget_set_sensitive(context_open_button_, count == 1U);
    }
    if (context_properties_button_ != nullptr) {
        gtk_widget_set_sensitive(context_properties_button_, count == 1U);
    }
}

void InteractionController::close_popovers()
{
    if (overflow_popover_ != nullptr) {
        gtk_popover_popdown(GTK_POPOVER(overflow_popover_));
    }
    if (context_popover_ != nullptr) {
        gtk_popover_popdown(GTK_POPOVER(context_popover_));
    }
}

void InteractionController::select_item_at_pointer(GtkWidget *view,
                                                   const double x,
                                                   const double y)
{
    GtkWidget *picked = gtk_widget_pick(view, x, y, GTK_PICK_DEFAULT);
    GtkWidget *named_tile = nullptr;

    for (GtkWidget *candidate = picked;
         candidate != nullptr && candidate != view;
         candidate = gtk_widget_get_parent(candidate)) {
        if (g_object_get_data(G_OBJECT(candidate), "ifm-name") != nullptr) {
            named_tile = candidate;
            break;
        }
        if (GtkWidget *descendant = find_descendant_with_name_label(candidate);
            descendant != nullptr) {
            named_tile = descendant;
            break;
        }
    }

    if (named_tile == nullptr) {
        gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(selection_));
        return;
    }

    auto *name_label = GTK_LABEL(
        g_object_get_data(G_OBJECT(named_tile), "ifm-name"));
    if (name_label == nullptr) {
        return;
    }
    const char *display_name = gtk_label_get_text(name_label);

    GListModel *model = gtk_multi_selection_get_model(selection_);
    if (model == nullptr) {
        return;
    }

    guint match = GTK_INVALID_LIST_POSITION;
    guint matches = 0U;
    const guint count = g_list_model_get_n_items(model);
    for (guint position = 0U; position < count; ++position) {
        GFileInfo *info = G_FILE_INFO(g_list_model_get_item(model, position));
        if (info != nullptr &&
            g_strcmp0(g_file_info_get_display_name(info), display_name) == 0) {
            match = position;
            ++matches;
        }
        if (info != nullptr) {
            g_object_unref(info);
        }
    }

    if (matches != 1U || match == GTK_INVALID_LIST_POSITION) {
        return;
    }

    if (!gtk_selection_model_is_selected(GTK_SELECTION_MODEL(selection_), match)) {
        gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(selection_));
        gtk_selection_model_select_item(GTK_SELECTION_MODEL(selection_), match, TRUE);
    }
}

void InteractionController::open_primary()
{
    if (primary_selection_ == nullptr || directory_list_ == nullptr) {
        return;
    }

    auto *info = G_FILE_INFO(
        gtk_single_selection_get_selected_item(primary_selection_));
    GFile *parent = gtk_directory_list_get_file(directory_list_);
    if (info == nullptr || parent == nullptr) {
        return;
    }

    const char *name = g_file_info_get_name(info);
    if (name == nullptr || name[0] == '\0') {
        return;
    }

    GFile *child = g_file_get_child(parent, name);
    if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
        char *parse_name = g_file_get_parse_name(child);
        if (parse_name != nullptr) {
            gtk_editable_set_text(GTK_EDITABLE(location_entry_), parse_name);
            g_signal_emit_by_name(location_entry_, "activate");
            g_free(parse_name);
        }
        g_object_unref(child);
        return;
    }

    char *uri = g_file_get_uri(child);
    g_object_unref(child);
    if (uri == nullptr) {
        return;
    }

    if (status_label_ != nullptr) {
        g_app_info_launch_default_for_uri_async(
            uri, nullptr, nullptr,
            &InteractionController::on_launch_finished,
            g_object_ref(status_label_));
    } else {
        GError *error = nullptr;
        g_app_info_launch_default_for_uri(uri, nullptr, &error);
        if (error != nullptr) {
            g_error_free(error);
        }
    }
    g_free(uri);
}


void InteractionController::inspect_primary()
{
    if (window_ == nullptr) return;
    auto *info = G_FILE_INFO(gtk_single_selection_get_selected_item(primary_selection_));
    GFile *parent = gtk_directory_list_get_file(directory_list_);
    if (info == nullptr || parent == nullptr || g_file_info_get_name(info) == nullptr) return;
    GFile *file = g_file_get_child(parent, g_file_info_get_name(info));
    show_file_inspection(window_, file, info);
    g_object_unref(file);
}

void InteractionController::compare_selected_hashes()
{
    if (window_ == nullptr || selection_count() != 2U) return;
    GFile *parent = gtk_directory_list_get_file(directory_list_);
    GListModel *model = gtk_multi_selection_get_model(selection_);
    if (parent == nullptr || model == nullptr) return;
    GtkBitset *bits = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(selection_));
    GtkBitsetIter iterator;
    guint position = 0;
    gboolean valid = gtk_bitset_iter_init_first(&iterator, bits, &position);
    std::vector<GFile *> files;
    while (valid) {
        auto *info = G_FILE_INFO(g_list_model_get_item(model, position));
        if (info != nullptr && g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR &&
            g_file_info_get_name(info) != nullptr)
            files.push_back(g_file_get_child(parent, g_file_info_get_name(info)));
        if (info != nullptr) g_object_unref(info);
        valid = gtk_bitset_iter_next(&iterator, &position);
    }
    gtk_bitset_unref(bits);
    if (files.size() == 2U) compare_file_hashes(window_, files[0], files[1]);
    for (GFile *file : files) g_object_unref(file);
}

void InteractionController::launch_terminal(bool selected)
{
    if (window_ == nullptr) return;
    GFile *parent = gtk_directory_list_get_file(directory_list_);
    if (parent == nullptr) return;
    GFile *directory = parent;
    if (selected) {
        auto *info = G_FILE_INFO(gtk_single_selection_get_selected_item(primary_selection_));
        if (info == nullptr || g_file_info_get_file_type(info) != G_FILE_TYPE_DIRECTORY ||
            g_file_info_get_name(info) == nullptr) return;
        directory = g_file_get_child(parent, g_file_info_get_name(info));
    }
    open_terminal_in_directory(window_, directory);
    if (selected) g_object_unref(directory);
}

void InteractionController::show_properties()
{
    auto *info = G_FILE_INFO(
        gtk_single_selection_get_selected_item(primary_selection_));
    GFile *parent = gtk_directory_list_get_file(directory_list_);
    if (info == nullptr || parent == nullptr) {
        return;
    }

    const char *name = g_file_info_get_name(info);
    if (name == nullptr || name[0] == '\0') {
        return;
    }

    GFile *child = g_file_get_child(parent, name);
    char *location = g_file_get_parse_name(child);

    GtkWidget *dialog = gtk_window_new();
    const char *display_name = g_file_info_get_display_name(info);
    const std::string title =
        std::string("Properties — ") +
        (display_name != nullptr ? display_name : name);
    gtk_window_set_title(GTK_WINDOW(dialog), title.c_str());
    gtk_window_set_transient_for(GTK_WINDOW(dialog), window_);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 520, -1);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_start(root, 18);
    gtk_widget_set_margin_end(root, 18);
    gtk_widget_set_margin_top(root, 18);
    gtk_widget_set_margin_bottom(root, 18);
    gtk_window_set_child(GTK_WINDOW(dialog), root);

    GtkWidget *heading = gtk_label_new(
        display_name != nullptr ? display_name : name);
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0F);
    gtk_widget_add_css_class(heading, "title-2");
    gtk_label_set_wrap(GTK_LABEL(heading), TRUE);
    gtk_box_append(GTK_BOX(root), heading);

    GtkWidget *grid_widget = gtk_grid_new();
    auto *grid = GTK_GRID(grid_widget);
    gtk_grid_set_row_spacing(grid, 9);
    gtk_grid_set_column_spacing(grid, 18);
    gtk_widget_set_hexpand(grid_widget, TRUE);
    gtk_box_append(GTK_BOX(root), grid_widget);

    int row = 0;
    add_property_row(grid, row++, "Type", detail_type_text(info));
    add_property_row(grid, row++, "Size", detail_size_text(info));
    add_property_row(grid, row++, "Modified", detail_modified_text(info));
    add_property_row(grid, row++, "Location",
                     location != nullptr ? location : "");

    const char *content_type = g_file_info_get_content_type(info);
    if (content_type != nullptr && content_type[0] != '\0') {
        add_property_row(grid, row++, "Content type", content_type);
    }
    if (g_file_info_has_attribute(info, "unix::blocks")) {
        const guint64 blocks = g_file_info_get_attribute_uint64(info, "unix::blocks");
        if (blocks <= G_MAXUINT64 / 512U) {
            char *size = g_format_size(blocks * 512U);
            add_property_row(grid, row++, "Allocated", size != nullptr ? size : "Unknown");
            g_free(size);
        }
    }
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_MODE)) {
        const guint32 mode = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE);
        char *permission = g_strdup_printf("%04o", mode & 07777U);
        add_property_row(grid, row++, "Permissions", permission != nullptr ? permission : "Unknown");
        g_free(permission);
    }
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_UID))
        add_property_row(grid, row++, "Owner ID", std::to_string(
            g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_UID)));
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_GID))
        add_property_row(grid, row++, "Group ID", std::to_string(
            g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_GID)));
    if (g_file_info_get_is_symlink(info)) {
        const char *target = g_file_info_get_symlink_target(info);
        add_property_row(grid, row++, "Link target",
                         target != nullptr ? target : "Unknown");
    }

    GtkWidget *close = gtk_button_new_with_label("Close");
    gtk_widget_set_halign(close, GTK_ALIGN_END);
    g_signal_connect_swapped(
        close, "clicked", G_CALLBACK(gtk_window_destroy), dialog);
    gtk_box_append(GTK_BOX(root), close);

    g_free(location);
    g_object_unref(child);
    gtk_window_present(GTK_WINDOW(dialog));
}

void InteractionController::rebuild_breadcrumbs()
{
    if (breadcrumb_box_ == nullptr) {
        return;
    }

    while (GtkWidget *child = gtk_widget_get_first_child(breadcrumb_box_)) {
        gtk_box_remove(GTK_BOX(breadcrumb_box_), child);
    }

    GFile *current = gtk_directory_list_get_file(directory_list_);
    if (current == nullptr) {
        GtkWidget *label = gtk_label_new("Location");
        gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_box_append(GTK_BOX(breadcrumb_box_), label);
        return;
    }

    std::vector<GFile *> chain;
    GFile *cursor = G_FILE(g_object_ref(current));
    while (cursor != nullptr) {
        chain.push_back(cursor);
        cursor = g_file_get_parent(chain.back());
    }
    std::reverse(chain.begin(), chain.end());

    constexpr std::size_t kMaximumVisibleSegments = 4U;
    const std::size_t start =
        chain.size() > kMaximumVisibleSegments
            ? chain.size() - kMaximumVisibleSegments
            : 0U;

    const auto append_crumb = [this, current](GFile *file, const char *label) {
        GtkWidget *button = gtk_button_new_with_label(label);
        gtk_widget_add_css_class(button, "flat");
        gtk_widget_add_css_class(button, "ifm-breadcrumb");
        if (g_file_equal(file, current))
            gtk_widget_add_css_class(button, "ifm-breadcrumb-current");
        gtk_widget_set_halign(button, GTK_ALIGN_START);
        char *uri = g_file_get_uri(file);
        g_object_set_data_full(G_OBJECT(button), "ifm-breadcrumb-target",
                               uri, g_free);
        g_signal_connect(button, "clicked",
                         G_CALLBACK(&InteractionController::on_breadcrumb_clicked), this);
        gtk_box_append(GTK_BOX(breadcrumb_box_), button);
    };

    if (start > 0U) {
        append_crumb(chain[start - 1U], "…");
        GtkWidget *separator = gtk_label_new("›");
        gtk_widget_add_css_class(separator, "dim-label");
        gtk_box_append(GTK_BOX(breadcrumb_box_), separator);
    }

    for (std::size_t index = start; index < chain.size(); ++index) {
        const std::string label = file_display_name(chain[index]);
        append_crumb(chain[index], label.empty() ? "/" : label.c_str());
        if (index + 1U < chain.size()) {
            GtkWidget *separator = gtk_label_new("›");
            gtk_widget_add_css_class(separator, "dim-label");
            gtk_box_append(GTK_BOX(breadcrumb_box_), separator);
        }
    }

    for (GFile *file : chain) {
        g_object_unref(file);
    }
}

void InteractionController::show_location_entry()
{
    if (title_stack_ == nullptr || location_entry_ == nullptr) {
        return;
    }
    gtk_stack_set_visible_child_name(GTK_STACK(title_stack_), "entry");
    gtk_widget_grab_focus(location_entry_);
    gtk_editable_select_region(GTK_EDITABLE(location_entry_), 0, -1);
}

void InteractionController::show_breadcrumbs()
{
    if (title_stack_ == nullptr) {
        return;
    }
    gtk_stack_set_visible_child_name(GTK_STACK(title_stack_), "breadcrumbs");
}

} // namespace infiltrator::files
