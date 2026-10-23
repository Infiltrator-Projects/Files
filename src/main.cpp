// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/file_manager_window.hpp"
#include "ui/theme_controller.hpp"

#include <gtk/gtk.h>

namespace {

infiltrator::files::FileManagerWindow *create_window(GtkApplication *application)
{
    auto *window = new infiltrator::files::FileManagerWindow(application);
    window->install_operation_surface();
    new infiltrator::files::ThemeController(window->native_window());
    window->present();
    return window;
}

void on_activate(GtkApplication *application, gpointer)
{
    const bool first = gtk_application_get_windows(application) == nullptr;
    auto *window = create_window(application);
    if (first) (void)window->restore_previous_session();
}

void on_open(GApplication *application, GFile **files, int count, const char *, gpointer)
{
    for (int i = 0; i < count; ++i)
        create_window(GTK_APPLICATION(application))->open_location(files[i]);
}

} // namespace

int main(int argc, char **argv)
{
    GtkApplication *application = gtk_application_new(
        "au.com.infiltrator.files", G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(application, "activate", G_CALLBACK(on_activate), nullptr);
    g_signal_connect(application, "open", G_CALLBACK(on_open), nullptr);
    const int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
