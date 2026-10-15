// SPDX-License-Identifier: GPL-3.0-or-later
#include "theme_controller.hpp"

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>

namespace infiltrator::files {

namespace {

std::string colour_hex(const std::uint32_t rgb)
{
    char buffer[8]{};
    std::snprintf(buffer, sizeof(buffer), "#%06X", rgb & 0x00FFFFFFU);
    return buffer;
}

char *theme_directory()
{
    return g_build_filename(g_get_user_config_dir(), "infiltrator-file-manager", nullptr);
}

char *theme_file()
{
    return g_build_filename(g_get_user_config_dir(), "infiltrator-file-manager", "theme", nullptr);
}

} // namespace

ThemeController::ThemeController(GtkWindow *window)
    : window_(window), mode_(load_mode())
{
    if (window_ == nullptr) {
        return;
    }

    settings_ = gtk_settings_get_default();
    if (settings_ != nullptr) {
        g_object_ref(settings_);
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(settings_), "gtk-theme-name") != nullptr) {
            theme_name_handler_ = g_signal_connect(
                settings_, "notify::gtk-theme-name",
                G_CALLBACK(&ThemeController::on_system_theme_changed), this);
        }
        if (g_object_class_find_property(
                G_OBJECT_GET_CLASS(settings_), "gtk-application-prefer-dark-theme") != nullptr) {
            prefer_dark_handler_ = g_signal_connect(
                settings_, "notify::gtk-application-prefer-dark-theme",
                G_CALLBACK(&ThemeController::on_system_theme_changed), this);
        }
    }

    display_ = gtk_widget_get_display(GTK_WIDGET(window_));
    if (display_ != nullptr) {
        g_object_ref(display_);
        provider_ = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(
            display_, GTK_STYLE_PROVIDER(provider_),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1U);
    }

    install_picker();
    apply_theme();
    g_object_weak_ref(G_OBJECT(window_), &ThemeController::on_window_finalized, this);
}

ThemeController::~ThemeController()
{
    if (settings_ != nullptr) {
        if (theme_name_handler_ != 0U) {
            g_signal_handler_disconnect(settings_, theme_name_handler_);
        }
        if (prefer_dark_handler_ != 0U) {
            g_signal_handler_disconnect(settings_, prefer_dark_handler_);
        }
        g_object_unref(settings_);
        settings_ = nullptr;
    }

    if (display_ != nullptr && provider_ != nullptr) {
        gtk_style_context_remove_provider_for_display(
            display_, GTK_STYLE_PROVIDER(provider_));
    }
    if (provider_ != nullptr) {
        g_object_unref(provider_);
        provider_ = nullptr;
    }
    if (display_ != nullptr) {
        g_object_unref(display_);
        display_ = nullptr;
    }
}

void ThemeController::on_window_finalized(gpointer user_data, GObject *where_object_was)
{
    (void)where_object_was;
    auto *self = static_cast<ThemeController *>(user_data);
    self->window_ = nullptr;
    delete self;
}

void ThemeController::on_theme_toggled(GtkCheckButton *button, gpointer user_data)
{
    if (!gtk_check_button_get_active(button)) {
        return;
    }

    auto *self = static_cast<ThemeController *>(user_data);
    const int value = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "ifm-theme-mode"));
    if (value < static_cast<int>(INFILTRATR_THEME_SYSTEM) ||
        value > static_cast<int>(INFILTRATR_THEME_NIGHT)) {
        return;
    }
    self->set_mode(static_cast<InfiltratrThemeMode>(value));
}

void ThemeController::on_system_theme_changed(GObject *object,
                                               GParamSpec *pspec,
                                               gpointer user_data)
{
    (void)object;
    (void)pspec;
    auto *self = static_cast<ThemeController *>(user_data);
    if (self->mode_ == INFILTRATR_THEME_SYSTEM) {
        self->apply_theme();
    }
}

void ThemeController::install_picker()
{
    GtkWidget *titlebar = gtk_window_get_titlebar(window_);
    if (!GTK_IS_HEADER_BAR(titlebar)) {
        return;
    }

    menu_button_ = gtk_menu_button_new();
    GtkWidget *icon = gtk_image_new_from_icon_name("preferences-system-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 17);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(menu_button_), icon);

    GtkWidget *popover = gtk_popover_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 10);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);

    constexpr const char *labels[] = {"Follow system", "Day", "Night"};
    for (std::size_t index = 0; index < mode_buttons_.size(); ++index) {
        GtkWidget *button = gtk_check_button_new_with_label(labels[index]);
        mode_buttons_[index] = button;
        g_object_set_data(
            G_OBJECT(button), "ifm-theme-mode",
            GINT_TO_POINTER(static_cast<int>(index)));
        if (index > 0U) {
            gtk_check_button_set_group(
                GTK_CHECK_BUTTON(button), GTK_CHECK_BUTTON(mode_buttons_[0]));
        }
        gtk_box_append(GTK_BOX(box), button);
    }

    const auto active_index = static_cast<std::size_t>(mode_);
    if (active_index < mode_buttons_.size()) {
        gtk_check_button_set_active(
            GTK_CHECK_BUTTON(mode_buttons_[active_index]), TRUE);
    }

    for (GtkWidget *button : mode_buttons_) {
        g_signal_connect(
            button, "toggled", G_CALLBACK(&ThemeController::on_theme_toggled), this);
    }

    gtk_popover_set_child(GTK_POPOVER(popover), box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu_button_), popover);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(titlebar), menu_button_);
    update_picker();
}

void ThemeController::apply_theme()
{
    if (provider_ == nullptr) {
        return;
    }

    const InfiltratrThemePalette *palette =
        infiltratr_theme_resolve(mode_, system_prefers_dark());
    if (palette == nullptr) {
        return;
    }

    std::ostringstream css;
    css << "window { background: " << colour_hex(palette->background_rgb)
        << "; color: " << colour_hex(palette->text_rgb) << "; }\n"
        << "headerbar { background: " << colour_hex(palette->titlebar_rgb)
        << "; color: " << colour_hex(palette->title_rgb)
        << "; border-bottom-color: " << colour_hex(palette->border_rgb) << "; }\n"
        << ".ifm-sidebar { background: " << colour_hex(palette->panel_rgb)
        << "; border-right-color: " << colour_hex(palette->border_rgb) << "; }\n"
        << ".ifm-sidebar row:hover { background: "
        << colour_hex(palette->surface_hover_rgb) << "; }\n"
        << ".ifm-file-list, .ifm-icon-grid { background: "
        << colour_hex(palette->background_rgb) << "; }\n"
        << ".ifm-file-list header, .ifm-file-list header button { background: "
        << colour_hex(palette->panel_rgb) << "; color: "
        << colour_hex(palette->muted_rgb) << "; border-color: "
        << colour_hex(palette->border_rgb) << "; }\n"
        << ".ifm-file-list row:hover, .ifm-icon-tile:hover { background: "
        << colour_hex(palette->surface_hover_rgb) << "; }\n"
        << ".ifm-file-list row:selected { background: "
        << colour_hex(palette->selection_background_rgb)
        << "; color: " << colour_hex(palette->selection_foreground_rgb) << "; }\n"
        << ".ifm-file-name { color: " << colour_hex(palette->text_rgb) << "; }\n"
        << ".ifm-detail-secondary { color: " << colour_hex(palette->muted_rgb) << "; }\n"
        << ".ifm-file-list row:selected .ifm-file-name, "
        << ".ifm-file-list row:selected .ifm-detail-secondary { color: "
        << colour_hex(palette->selection_foreground_rgb) << "; }\n"
        << ".ifm-status { background: " << colour_hex(palette->panel_rgb)
        << "; color: " << colour_hex(palette->muted_rgb)
        << "; border-top-color: " << colour_hex(palette->border_rgb) << "; }\n"
        << "entry { background: " << colour_hex(palette->input_rgb)
        << "; color: " << colour_hex(palette->text_rgb) << "; }\n"
        << "button, menubutton { color: " << colour_hex(palette->text_rgb) << "; }\n"
        << "popover { background: " << colour_hex(palette->card_rgb)
        << "; color: " << colour_hex(palette->text_rgb) << "; }\n";

    const std::string stylesheet = css.str();
    gtk_css_provider_load_from_data(
        provider_, stylesheet.c_str(), static_cast<gssize>(stylesheet.size()));
}

void ThemeController::set_mode(const InfiltratrThemeMode mode)
{
    if (mode_ == mode) {
        update_picker();
        return;
    }

    mode_ = mode;
    save_mode(mode_);
    apply_theme();
    update_picker();
}

void ThemeController::update_picker()
{
    if (menu_button_ == nullptr) {
        return;
    }

    std::string tooltip = "Appearance: ";
    tooltip += mode_label(mode_);
    gtk_widget_set_tooltip_text(menu_button_, tooltip.c_str());
}

bool ThemeController::system_prefers_dark() const
{
    if (settings_ == nullptr) {
        return false;
    }

    gboolean prefer_dark = FALSE;
    if (g_object_class_find_property(
            G_OBJECT_GET_CLASS(settings_), "gtk-application-prefer-dark-theme") != nullptr) {
        g_object_get(settings_, "gtk-application-prefer-dark-theme", &prefer_dark, nullptr);
        if (prefer_dark != FALSE) {
            return true;
        }
    }

    gchar *theme_name = nullptr;
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(settings_), "gtk-theme-name") != nullptr) {
        g_object_get(settings_, "gtk-theme-name", &theme_name, nullptr);
    }
    if (theme_name == nullptr) {
        return false;
    }

    gchar *lower = g_ascii_strdown(theme_name, -1);
    const bool dark = lower != nullptr && g_strstr_len(lower, -1, "dark") != nullptr;
    g_free(lower);
    g_free(theme_name);
    return dark;
}

InfiltratrThemeMode ThemeController::load_mode()
{
    char *path = theme_file();
    gchar *contents = nullptr;
    gsize length = 0U;
    if (!g_file_get_contents(path, &contents, &length, nullptr)) {
        g_free(path);
        return INFILTRATR_THEME_SYSTEM;
    }
    g_free(path);

    g_strstrip(contents);
    InfiltratrThemeMode mode = INFILTRATR_THEME_SYSTEM;
    const bool valid = infiltratr_theme_mode_parse(contents, &mode);
    g_free(contents);
    return valid ? mode : INFILTRATR_THEME_SYSTEM;
}

void ThemeController::save_mode(const InfiltratrThemeMode mode)
{
    char *directory = theme_directory();
    if (g_mkdir_with_parents(directory, 0700) != 0) {
        g_free(directory);
        return;
    }

    char *path = theme_file();
    const std::string value = std::string(infiltratr_theme_mode_key(mode)) + "\n";
    (void)g_file_set_contents(
        path, value.c_str(), static_cast<gssize>(value.size()), nullptr);
    g_free(path);
    g_free(directory);
}

const char *ThemeController::mode_label(const InfiltratrThemeMode mode) noexcept
{
    switch (mode) {
    case INFILTRATR_THEME_DAY:
        return "Day";
    case INFILTRATR_THEME_NIGHT:
        return "Night";
    case INFILTRATR_THEME_SYSTEM:
    default:
        return "Follow system";
    }
}

} // namespace infiltrator::files
