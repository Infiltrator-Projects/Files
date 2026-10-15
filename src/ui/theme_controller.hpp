// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <infiltratr/design.h>

#include <gtk/gtk.h>

#include <array>

namespace infiltrator::files {

class ThemeController final {
public:
    explicit ThemeController(GtkWindow *window);
    ~ThemeController();

    ThemeController(const ThemeController &) = delete;
    ThemeController &operator=(const ThemeController &) = delete;

private:
    static void on_window_finalized(gpointer user_data, GObject *where_object_was);
    static void on_theme_toggled(GtkCheckButton *button, gpointer user_data);
    static void on_system_theme_changed(GObject *object, GParamSpec *pspec, gpointer user_data);

    void install_picker();
    void apply_theme();
    void set_mode(InfiltratrThemeMode mode);
    void update_picker();
    [[nodiscard]] bool system_prefers_dark() const;

    [[nodiscard]] static InfiltratrThemeMode load_mode();
    static void save_mode(InfiltratrThemeMode mode);
    [[nodiscard]] static const char *mode_label(InfiltratrThemeMode mode) noexcept;

    GtkWindow *window_{nullptr};
    GtkSettings *settings_{nullptr};
    GdkDisplay *display_{nullptr};
    GtkCssProvider *provider_{nullptr};
    GtkWidget *menu_button_{nullptr};
    std::array<GtkWidget *, 3> mode_buttons_{};
    gulong theme_name_handler_{0};
    gulong prefer_dark_handler_{0};
    InfiltratrThemeMode mode_{INFILTRATR_THEME_SYSTEM};
};

} // namespace infiltrator::files
