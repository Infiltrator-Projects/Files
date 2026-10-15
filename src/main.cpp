// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/file_manager_window.hpp"
#include "ui/theme_controller.hpp"

#include <gtk/gtk.h>

namespace {

void on_activate(GtkApplication *application, gpointer user_data)
{
    (void)user_data;
    auto *window = new infiltrator::files::FileManagerWindow(application);
    window->install_operation_surface();
    new infiltrator::files::ThemeController(window->native_window());
    window->present();
}

} // namespace

int main(int argc, char **argv)
{
    GtkApplication *application = gtk_application_new(
        "au.com.infiltrator.files", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(on_activate), nullptr);
    const int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
