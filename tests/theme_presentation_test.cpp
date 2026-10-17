// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/ui/file_manager_window.hpp"
#include "../src/ui/theme_controller.hpp"

#include <infiltratr/design.h>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace infiltrator::files {
struct ThemeControllerTestAccess {
    static void set(ThemeController &theme, InfiltratrThemeMode mode)
    {
        theme.set_mode(mode);
    }
};
} // namespace infiltrator::files

using namespace infiltrator::files;

namespace {
void require(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}

void settle()
{
    for (int i = 0; i < 64; ++i)
        while (g_main_context_iteration(nullptr, FALSE)) {}
}

bool style_matches(GtkWidget *widget, std::uint32_t rgb)
{
    GdkRGBA foreground{};
    gtk_style_context_get_color(gtk_widget_get_style_context(widget), &foreground);
    const auto near = [](double actual, std::uint32_t part) {
        return std::abs(actual - static_cast<double>(part) / 255.0) < 0.025;
    };
    return near(foreground.red, (rgb >> 16U) & 255U) &&
           near(foreground.green, (rgb >> 8U) & 255U) &&
           near(foreground.blue, rgb & 255U);
}
}

int main()
{
    char *temporary = g_dir_make_tmp("files-theme-XXXXXX", nullptr);
    require(temporary != nullptr, "Cannot create isolated settings root");
    const std::filesystem::path root(temporary);
    g_free(temporary);
    const auto config = root / "config";
    g_setenv("XDG_CONFIG_HOME", config.c_str(), TRUE);
    g_setenv("GTK_A11Y", "none", TRUE);
    g_setenv("GSETTINGS_BACKEND", "memory", TRUE);

    gtk_init();
    GtkApplication *application = gtk_application_new(
        "org.infiltrator.Files.ThemeTest", G_APPLICATION_NON_UNIQUE);
    require(g_application_register(G_APPLICATION(application), nullptr, nullptr),
            "Cannot register test application");

    try {
        auto *first = new FileManagerWindow(application);
        auto *second = new FileManagerWindow(application);
        auto *day = new ThemeController(first->native_window());
        auto *night = new ThemeController(second->native_window());
        first->present();
        second->present();
        settle();

        // The initial window title reflects the filesystem root path.
        const std::string expected = "Files — /";
        require(expected == gtk_window_get_title(first->native_window()),
                "Files must start at filesystem root, not the login home");

        ThemeControllerTestAccess::set(*day, INFILTRATR_THEME_DAY);
        ThemeControllerTestAccess::set(*night, INFILTRATR_THEME_NIGHT);
        settle();

        const auto *day_palette = infiltratr_theme_resolve(INFILTRATR_THEME_DAY, false);
        const auto *night_palette = infiltratr_theme_resolve(INFILTRATR_THEME_NIGHT, true);
        require(day_palette != nullptr && night_palette != nullptr,
                "Common theme palettes must resolve");

        GtkWidget *first_header = gtk_window_get_titlebar(first->native_window());
        GtkWidget *second_header = gtk_window_get_titlebar(second->native_window());
        require(style_matches(first_header, day_palette->title_rgb),
                "Day header must use legible Common Day title foreground");
        require(style_matches(second_header, night_palette->title_rgb),
                "Night header must use legible Common Night title foreground");

        ThemeControllerTestAccess::set(*day, INFILTRATR_THEME_NIGHT);
        ThemeControllerTestAccess::set(*night, INFILTRATR_THEME_DAY);
        settle();
        require(style_matches(first_header, night_palette->title_rgb),
                "Changing the first window's palette must repaint its header");
        require(style_matches(second_header, day_palette->title_rgb),
                "Changing the second window's palette must not leak between windows");

        gtk_window_destroy(first->native_window());
        gtk_window_destroy(second->native_window());
        settle();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        g_object_unref(application);
        return 1;
    }
    g_object_unref(application);
    std::filesystem::remove_all(root);
    return 0;
}
