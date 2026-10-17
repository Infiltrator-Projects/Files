// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/ui/file_manager_window.hpp"

#include <gtk/gtk.h>

#include <iostream>
#include <stdexcept>

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
        require(find_button(popover, "New Folder") != nullptr,
                "New Folder must remain available after header consolidation");
        require(find_button(popover, "Properties") != nullptr,
                "Properties must be available from File actions");

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
