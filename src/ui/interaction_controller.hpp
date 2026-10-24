// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <gtk/gtk.h>

#include <vector>

namespace infiltrator::files {

class InteractionController final {
public:
    InteractionController(GtkWindow *window,
                          GtkDirectoryList *directory_list,
                          GtkMultiSelection *selection,
                          GtkSingleSelection *primary_selection,
                          GtkWidget *location_entry,
                          GtkWidget *content_stack,
                          GtkWidget *status_label);
    ~InteractionController();

    InteractionController(const InteractionController &) = delete;
    InteractionController &operator=(const InteractionController &) = delete;

private:
    enum class ActionScope {
        Always,
        NoSelection,
        SingleSelection,
        MultiSelection,
        AnySelection,
    };

    struct ProxyAction {
        GtkWidget *button{nullptr};
        GtkWidget *target{nullptr};
        ActionScope scope{ActionScope::Always};
        bool requires_target{true};
    };

    static void on_window_finalized(gpointer user_data, GObject *where_object_was);
    static void on_proxy_clicked(GtkButton *button, gpointer user_data);
    static void on_open_clicked(GtkButton *button, gpointer user_data);
    static void on_workspace_clicked(GtkButton *button, gpointer user_data);
    static void on_properties_clicked(GtkButton *button, gpointer user_data);
    static void on_edit_location_clicked(GtkButton *button, gpointer user_data);
    static void on_breadcrumb_clicked(GtkButton *button, gpointer user_data);
    static void on_selection_changed(GtkSelectionModel *model,
                                     guint position,
                                     guint n_items,
                                     gpointer user_data);
    static void on_location_changed(GObject *object, GParamSpec *pspec, gpointer user_data);
    static gboolean on_key_pressed(GtkEventControllerKey *controller,
                                   guint keyval,
                                   guint keycode,
                                   GdkModifierType state,
                                   gpointer user_data);
    static void on_context_pressed(GtkGestureClick *gesture,
                                   int n_press,
                                   double x,
                                   double y,
                                   gpointer user_data);
    static void on_launch_finished(GObject *source, GAsyncResult *result, gpointer user_data);

    void install_breadcrumb_surface();
    void install_action_surfaces();
    void install_context_gestures();
    void install_keyboard_shortcuts();
    void discover_existing_actions();
    void rebuild_breadcrumbs();
    void show_location_entry();
    void show_breadcrumbs();
    void update_action_state();
    void close_popovers();
    void select_item_at_pointer(GtkWidget *view, double x, double y);
    void open_primary();
    void show_open_with();
    void show_properties();
    void inspect_primary();
    void show_tags_dialog();
    void compare_selected_hashes();
    void launch_terminal(bool selected);
    void show_selection_pattern_dialog();
    void select_same_type();
    void copy_selected_text(bool uris);

    GtkWidget *create_action_button(GtkWidget *box,
                                    const char *label,
                                    GtkWidget *target,
                                    ActionScope scope,
                                    std::vector<ProxyAction> &proxies,
                                    GCallback callback = nullptr);
    GtkWidget *add_command(GtkWidget *box, const char *label, const char *command,
                           ActionScope scope, std::vector<ProxyAction> &proxies);
    GtkWidget *build_action_box(std::vector<ProxyAction> &proxies,
                                GtkWidget **open_button,
                                GtkWidget **properties_button);
    GtkWidget *build_context_action_box();
    [[nodiscard]] guint selection_count() const;

    GtkWindow *window_{nullptr};
    GtkDirectoryList *directory_list_{nullptr};
    GtkMultiSelection *selection_{nullptr};
    GtkSingleSelection *primary_selection_{nullptr};
    GtkWidget *location_entry_{nullptr};
    GtkWidget *content_stack_{nullptr};
    GtkWidget *status_label_{nullptr};

    GtkWidget *title_root_{nullptr};
    GtkWidget *title_stack_{nullptr};
    GtkWidget *breadcrumb_box_{nullptr};
    GtkWidget *edit_location_button_{nullptr};

    GtkWidget *overflow_menu_button_{nullptr};
    GtkWidget *overflow_popover_{nullptr};
    GtkWidget *context_popover_{nullptr};
    GtkWidget *overflow_open_button_{nullptr};
    GtkWidget *overflow_properties_button_{nullptr};
    GtkWidget *context_open_button_{nullptr};
    GtkWidget *context_properties_button_{nullptr};

    GtkWidget *new_folder_target_{nullptr};
    GtkWidget *new_file_target_{nullptr};
    std::vector<GtkWidget *> open_with_actions_;
    GtkWidget *rename_target_{nullptr};
    GtkWidget *copy_target_{nullptr};
    GtkWidget *move_target_{nullptr};
    GtkWidget *batch_copy_target_{nullptr};
    GtkWidget *batch_move_target_{nullptr};
    GtkWidget *trash_target_{nullptr};
    GtkWidget *restore_target_{nullptr};
    GtkWidget *delete_target_{nullptr};

    std::vector<GtkWidget *> open_in_tab_actions_;
    std::vector<GtkWidget *> open_in_window_actions_;
    std::vector<GtkWidget *> hidden_actions_;
    std::vector<GtkWidget *> extension_actions_;
    std::vector<GtkWidget *> folder_sort_actions_;
    std::vector<GtkWidget *> sidebar_actions_;
    std::vector<GtkWidget *> folder_terminal_actions_;
    std::vector<GtkWidget *> compare_actions_;
    std::vector<GtkWidget *> preview_actions_;
    std::vector<ProxyAction> overflow_proxies_;
    std::vector<ProxyAction> context_proxies_;
    std::vector<GtkWidget *> context_sections_;

    gulong selection_handler_{0U};
    gulong location_handler_{0U};
};

} // namespace infiltrator::files
