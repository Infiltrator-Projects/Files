// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/ui/file_manager_window.hpp"
#include "../src/ui/theme_controller.hpp"

#include <infiltratr/design.h>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

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

GtkWidget *find_css_class(GtkWidget *root, const char *name)
{
    if (root == nullptr) return nullptr;
    if (gtk_widget_has_css_class(root, name)) return root;
    for (GtkWidget *child = gtk_widget_get_first_child(root); child != nullptr;
         child = gtk_widget_get_next_sibling(child)) {
        if (GtkWidget *found = find_css_class(child, name); found != nullptr) return found;
    }
    return nullptr;
}

// Observe the actual GTK4 ColumnView row allocation, not merely its CSS
// min-height. Native theme row/cell insets previously produced 45px rows.
std::pair<int, int> row_geometry(GtkWidget *widget)
{
    if (widget == nullptr) return {0, 0};
    if (g_strcmp0(gtk_widget_get_css_name(widget), "row") == 0 &&
        gtk_widget_get_height(widget) > 0) {
        return {1, gtk_widget_get_height(widget)};
    }
    std::pair<int, int> geometry{0, 0};
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child != nullptr;
         child = gtk_widget_get_next_sibling(child)) {
        const auto next = row_geometry(child);
        geometry.first += next.first;
        geometry.second = std::max(geometry.second, next.second);
    }
    return geometry;
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

        // These actual widget colours must follow Common semantic roles, rather
        // than inheriting unreadable desktop theme defaults. Each window's
        // stylesheet must remain isolated when Day and Night coexist.
        GtkWidget *day_status = find_css_class(GTK_WIDGET(first->native_window()),
                                                "ifm-status-text");
        GtkWidget *night_status = find_css_class(GTK_WIDGET(second->native_window()),
                                                  "ifm-status-text");
        GtkWidget *day_sidebar = find_css_class(GTK_WIDGET(first->native_window()),
                                                 "ifm-sidebar");
        GtkWidget *night_sidebar = find_css_class(GTK_WIDGET(second->native_window()),
                                                   "ifm-sidebar");
        GtkWidget *day_brand = find_css_class(GTK_WIDGET(first->native_window()),
                                               "ifm-brand-title");
        GtkWidget *night_brand = find_css_class(GTK_WIDGET(second->native_window()),
                                                 "ifm-brand-title");
        require(day_status != nullptr && night_status != nullptr &&
                day_sidebar != nullptr && night_sidebar != nullptr &&
                day_brand != nullptr && night_brand != nullptr,
                "Files styling requires the canonical sidebar, status and brand surfaces");
        GtkWidget *day_list = find_css_class(GTK_WIDGET(first->native_window()),
                                              "ifm-file-list");
        GtkWidget *night_list = find_css_class(GTK_WIDGET(second->native_window()),
                                                "ifm-file-list");
        require(day_list != nullptr && night_list != nullptr,
                "The detail list must be materialised in both windows");
        const auto day_rows = row_geometry(day_list);
        const auto night_rows = row_geometry(night_list);
        if (day_rows.first == 0 || night_rows.first == 0 ||
            day_rows.second > 38 || night_rows.second > 38) {
            std::cerr << "GTK details row geometry: Day=" << day_rows.first
                      << " rows, " << day_rows.second << "px; Night="
                      << night_rows.first << " rows, " << night_rows.second
                      << "px (target: <=38px at test DPI)\n";
            throw std::runtime_error("Details row density exceeds the compact layout");
        }
        require(style_matches(day_status, day_palette->summary_rgb) &&
                style_matches(night_status, night_palette->summary_rgb),
                "Status text must use Common's readable summary text in both themes");
        require(style_matches(day_sidebar, day_palette->text_rgb) &&
                style_matches(night_sidebar, night_palette->text_rgb),
                "Sidebar foreground must use the Common primary text role");
        require(style_matches(day_brand, day_palette->title_rgb) &&
                style_matches(night_brand, night_palette->title_rgb),
                "Brand title must use the Common title role");

        ThemeControllerTestAccess::set(*day, INFILTRATR_THEME_NIGHT);
        ThemeControllerTestAccess::set(*night, INFILTRATR_THEME_DAY);
        settle();
        require(style_matches(first_header, night_palette->title_rgb),
                "Changing the first window's palette must repaint its header");
        require(style_matches(second_header, day_palette->title_rgb),
                "Changing the second window's palette must not leak between windows");
        require(style_matches(day_status, night_palette->summary_rgb) &&
                style_matches(night_status, day_palette->summary_rgb),
                "Status text must change with its own window and never another window");
        require(style_matches(day_sidebar, night_palette->text_rgb) &&
                style_matches(night_sidebar, day_palette->text_rgb),
                "Sidebar text must continue matching its own window theme");

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
