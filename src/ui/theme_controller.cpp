// SPDX-License-Identifier: GPL-3.0-or-later
#include "theme_controller.hpp"

#include <infiltratr/posix.h>

#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

std::filesystem::path theme_directory()
{
    char *config_home = nullptr;
    if (!infiltratr_xdg_config_home_alloc(&config_home) || config_home == nullptr) {
        return {};
    }
    const std::filesystem::path directory =
        std::filesystem::path(config_home) / "files";
    std::free(config_home);
    return directory;
}

std::filesystem::path theme_file()
{
    const std::filesystem::path directory = theme_directory();
    return directory.empty() ? std::filesystem::path{} : directory / "theme";
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

    // A per-window class prevents a Day window's stylesheet from changing a
    // separate Night window. Unlike the old global frozen provider, it is
    // removed when this controller/window is destroyed.
    static std::atomic<unsigned> next_scope{0U};
    scope_class_ = "ifm-theme-" + std::to_string(++next_scope);
    gtk_widget_add_css_class(GTK_WIDGET(window_), scope_class_.c_str());

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

    if (window_ != nullptr && !scope_class_.empty())
        gtk_widget_remove_css_class(GTK_WIDGET(window_), scope_class_.c_str());
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
    if (provider_ == nullptr || scope_class_.empty()) return;

    const bool system_dark = system_prefers_dark();
    const bool dark = mode_ == INFILTRATR_THEME_NIGHT ||
        (mode_ == INFILTRATR_THEME_SYSTEM && system_dark);
    const InfiltratrThemePalette *palette =
        infiltratr_theme_resolve(mode_, system_dark);
    const InfiltratrDesignMetrics *metrics = infiltratr_design_metrics();
    const InfiltratrTypography *typography = infiltratr_typography();
    if (palette == nullptr || metrics == nullptr || typography == nullptr) return;

    // Common owns the semantic colour/metric/typography contract. Files owns
    // only the density and placement of accents. No ad-hoc Day/Night RGB values.
    const std::string background = colour_hex(palette->background_rgb);
    const std::string panel = colour_hex(palette->panel_rgb);
    const std::string card = colour_hex(palette->card_rgb);
    const std::string surface = colour_hex(palette->surface_rgb);
    const std::string input = colour_hex(palette->input_rgb);
    const std::string border = colour_hex(palette->border_rgb);
    const std::string text = colour_hex(palette->text_rgb);
    const std::string muted = colour_hex(palette->muted_rgb);
    const std::string secondary = colour_hex(palette->summary_rgb);
    const std::string titlebar = colour_hex(palette->titlebar_rgb);
    const std::string title = colour_hex(palette->title_rgb);
    const std::string hover = colour_hex(palette->surface_hover_rgb);
    const std::string selected = colour_hex(palette->selection_background_rgb);
    const std::string selected_text = colour_hex(palette->selection_foreground_rgb);
    const std::string accent = colour_hex(palette->neutral_accent_rgb);
    const std::string accent_text = colour_hex(palette->info_rgb);
    const std::string active_hover = colour_hex(palette->card_hover_rgb);
    const std::string subtle_border = colour_hex(palette->status_border_rgb);
    const std::string radius = std::to_string(metrics->control_radius) + "px";
    // Common's global background is deliberately near-black at Night. The
    // browsing workspace is a *surface*, not the surrounding page background.
    // At Day, the lighter card role gives the white rows a calm neutral frame.
    const std::string browsing_surface = dark ? surface : card;
    const std::string sidebar_surface = dark ? panel : surface;

    std::ostringstream css;
    const std::string scope = "." + scope_class_;
    const auto rule = [&](const char *selector, const std::string &declarations) {
        css << scope << " " << selector << " { " << declarations << " }\n";
    };
    const auto rules = [&](std::initializer_list<const char *> selectors,
                           const std::string &declarations) {
        for (const char *selector : selectors) rule(selector, declarations);
    };

    css << scope << " { background-color: " << background << "; color: " << text
        << "; font-family: 'MB Corpo S Text WEB', '" << typography->ui_family
        << "', " << typography->gtk_fallback << "; }\n";

    // Explicitly colour header descendants: native window controls and
    // breadcrumb text otherwise inherit a low-contrast desktop GTK theme.
    rule("headerbar", "background-color:" + titlebar + ";color:" + title +
         ";border-bottom:1px solid " + subtle_border +
         ";min-height:44px;padding:2px 8px");
    rules({"headerbar button", "headerbar menubutton", "headerbar windowcontrols",
           "headerbar windowcontrols button", "headerbar image", "headerbar label"},
          "color:" + title);
    rule("headerbar button", "background-color:transparent;border-radius:" + radius +
         ";min-height:30px");
    rule("headerbar button:hover", "background-color:" + hover + ";color:" + text);
    rule("headerbar button:disabled", "color:" + muted);
    rule("headerbar .ifm-brand", "margin-right:12px");
    rule("headerbar .ifm-brand-title", "color:" + title +
         ";font-family:'" + typography->brand_family +
         "';font-weight:" + std::to_string(typography->brand_weight) +
         ";font-size:1.2em");
    rule("headerbar .ifm-brand-mark", "color:" + accent);
    rule("headerbar .ifm-breadcrumb", "color:" + title +
         ";padding:3px 9px;min-height:28px;"
         "font-family:'MB Corpo A Text Cond WEB','MB Corpo A Title Cond WEB'");
    rule("headerbar .ifm-breadcrumb-current", "background-color:" + card +
         ";border:1px solid " + accent + ";color:" + title +
         ";font-weight:650");
    rule("headerbar .ifm-breadcrumb:hover", "background-color:" + hover);
    rule("headerbar entry", "background-color:" + input + ";color:" + text +
         ";border:1px solid " + border + ";border-radius:" + radius);

    // GTK4 renders ColumnView through *nested* listview/row/cell CSS nodes.
    // Target the real nodes, not only a class on the outer ColumnView: this
    // prevents dark-theme row backgrounds persisting inside the Day UI.
    const std::string browsing = "background-color:" + browsing_surface + ";color:" + text;
    rules({".ifm-content-stack", ".ifm-browser-scroll", ".ifm-browser-scroll viewport",
           "columnview", "columnview listview", "columnview listview.view",
           "gridview", "gridview.view", "listview.view"},
          browsing);
    // GTK themes may add row and cell padding *on top* of our item factory's
    // padding. The old min-height:31px alone allowed 45px rows in Mint.
    // Zero the native row/cell insets; the 20px icon plus 2px factory margins
    // now determines the Details row's useful height (about 32px at 100%).
    rules({"columnview listview row", "columnview row"},
          "background-color:" + browsing_surface + ";color:" + text +
          ";border-radius:0;min-height:30px;padding:0;margin:0");
    rule("columnview listview row cell",
         "padding:0;margin:0;min-height:0;border:0");
    rule("gridview child", "background-color:" + browsing_surface +
         ";color:" + text + ";border-radius:" + radius);
    rules({"columnview row:hover", "columnview listview row:hover", "gridview child:hover"},
          "background-color:" + hover + ";color:" + text);
    rules({"columnview row:selected", "columnview listview row:selected",
           "gridview child:selected"},
          "background-color:" + selected + ";color:" + selected_text);
    rule("columnview cell", "background-color:transparent;border:0;padding:0;margin:0");
    rule(".ifm-detail-cell", "min-height:24px");
    rule(".ifm-file-list", "border:0");
    rules({"columnview row:selected", "columnview listview row:selected"},
          "box-shadow:inset 3px 0 " + accent);
    rules({"columnview row:focus-visible", "gridview child:focus-visible"},
          "outline:2px solid " + accent + ";outline-offset:-2px");
    rules({"columnview header", "columnview header button",
           "columnview header label"},
          "background-color:" + panel + ";color:" + text +
          ";border-color:" + subtle_border + ";font-size:1.00em;font-weight:700;"
          "font-family:'MB Corpo S Text WEB Cond','MB Corpo S Title WEB'");
    rule("columnview header button", "min-height:30px;padding:3px 10px");
    rule("columnview header button:hover", "background-color:" + hover +
         ";color:" + text);
    rules({".ifm-file-name", ".ifm-icon-tile .ifm-file-name"},
          "color:" + text + ";font-weight:400;font-size:1.05em;"
          "font-family:'MB Corpo S Text WEB','MB Corpo S Title WEB'");
    rule(".ifm-detail-secondary", "color:" + secondary + ";font-size:1.00em;"
         "font-family:'MB Corpo S Text WEB Cond','MB Corpo S Title WEB'");
    rules({"columnview row:selected .ifm-file-name",
           "columnview row:selected .ifm-detail-secondary",
           "gridview child:selected .ifm-file-name"},
          "color:" + selected_text);
    rule(".ifm-icon-tile", "padding:7px 8px;margin:3px;border-radius:" + radius);
    rule(".ifm-compact-tile", "padding:4px 7px;margin:2px");
    rule(".ifm-empty-state", "color:" + secondary + ";padding:28px");
    rule(".ifm-empty-state .title-3", "color:" + text + ";font-weight:600");

    // One restrained blue/cyan accent, borrowed from Common's info role.
    // The file content remains neutral and easy to scan; no dashboard cards.
    rule(".ifm-sidebar", "background-color:" + sidebar_surface + ";color:" + text +
         ";border-right:1px solid " + subtle_border);
    rule(".ifm-sidebar row", "color:" + text +
         ";background-color:transparent;margin:1px 7px;border-radius:" + radius +
         ";min-height:32px");
    rule(".ifm-sidebar row:hover", "background-color:" + hover);
    rule(".ifm-sidebar row.ifm-place-current", "background-color:" + selected +
         ";color:" + selected_text + ";border-left:3px solid " + accent +
         ";font-weight:650");
    rule(".ifm-sidebar row label", "color:" + text +
         ";font-family:'MB Corpo S Text WEB','MB Corpo S Title WEB'");
    rule(".ifm-sidebar row.ifm-place-current label", "color:" + selected_text);
    rule(".ifm-sidebar row image", "color:" + muted);
    rule(".ifm-sidebar row.ifm-place-current image", "color:" + accent_text);
    rule(".ifm-section-caption", "color:" + secondary +
         ";font-size:0.90em;font-weight:700;letter-spacing:0.08em;"
         "font-family:'MB Corpo S Title WEB Cond','MB Corpo S Title WEB'");
    rule(".ifm-sidebar row:focus-visible", "outline:2px solid " + accent);

    // The header and tab rail form one navigation surface, separated by a
    // fine Common border rather than by mismatched theme-native buttons.
    rule(".ifm-tab-line", "background-color:" + titlebar +
         ";border-bottom:1px solid " + subtle_border + ";min-height:38px");
    rule(".ifm-tab-group", "margin:3px 2px;border-radius:" + radius);
    rule(".ifm-tab-button", "color:" + secondary +
         ";background-color:transparent;border:1px solid transparent;"
         "border-radius:" + radius + ";padding:3px 11px;min-height:29px");
    rule(".ifm-tab-button:hover", "background-color:" + hover + ";color:" + text);
    rule(".ifm-tab-active", "background-color:" + browsing_surface + ";color:" + text +
         ";border:1px solid " + subtle_border + ";border-bottom:2px solid " + accent +
         ";font-weight:650");
    rule(".ifm-tab-close", "color:" + muted +
         ";background-color:transparent;border-radius:" + radius);
    rule(".ifm-tab-close:hover", "color:" + text + ";background-color:" + hover);
    rule(".ifm-tab-line button.flat", "color:" + text);

    rule(".ifm-status", "background-color:" + panel + ";color:" + secondary +
         ";border-top:1px solid " + subtle_border + ";min-height:37px");
    rule(".ifm-status-text", "color:" + secondary + ";font-size:1.00em");
    // Native GTK linked/toggle button backgrounds were leaking a dark fill
    // into unselected Day controls. Set the full state, not just its text.
    rule(".ifm-view-switcher", "background-color:" + browsing_surface +
         ";border:1px solid " + subtle_border + ";border-radius:" + radius);
    rule(".ifm-view-switcher button.ifm-view-button",
         "background-color:transparent;background-image:none;box-shadow:none;"
         "color:" + secondary + ";border:1px solid transparent;"
         "border-radius:" + radius + ";min-width:32px;min-height:30px;padding:4px");
    rule(".ifm-view-switcher button.ifm-view-button:hover",
         "background-color:" + hover + ";color:" + text);
    rule(".ifm-view-switcher button.ifm-view-button:checked",
         "background-color:" + active_hover + ";color:" + text +
         ";border:1px solid " + accent);
    rule(".ifm-view-switcher button.ifm-view-button:checked:hover",
         "background-color:" + active_hover + ";color:" + text);

    rules({"popover", "popover contents"}, "background-color:" + card +
          ";color:" + text + ";border:1px solid " + border);
    rule("popover button", "color:" + text + ";border-radius:" + radius);
    rule("popover button:hover", "background-color:" + active_hover);
    rule("popover label", "color:" + text);
    rule("entry", "background-color:" + input + ";color:" + text +
         ";border-radius:" + radius);
    rule("entry selection", "background-color:" + selected +
         ";color:" + selected_text);
    rules({"button:focus-visible", "menubutton:focus-visible",
           "entry:focus-visible", "listview child:focus-visible"},
          "outline:2px solid " + accent + ";outline-offset:1px");

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
    const std::filesystem::path path = theme_file();
    if (path.empty()) {
        return INFILTRATR_THEME_SYSTEM;
    }

    char contents[64]{};
    if (infiltratr_read_text_file_ex(path.c_str(), contents, sizeof(contents), nullptr) !=
        INFILTRATR_IO_OK) {
        return INFILTRATR_THEME_SYSTEM;
    }

    InfiltratrThemeMode mode = INFILTRATR_THEME_SYSTEM;
    return infiltratr_theme_mode_parse(contents, &mode) ? mode : INFILTRATR_THEME_SYSTEM;
}

void ThemeController::save_mode(const InfiltratrThemeMode mode)
{
    const std::filesystem::path directory = theme_directory();
    if (directory.empty() || infiltratr_mkdir_parents(directory.c_str(), 0700U) != 0) {
        return;
    }

    const std::filesystem::path path = directory / "theme";
    const std::string value = std::string(infiltratr_theme_mode_key(mode)) + "\n";
    (void)infiltratr_atomic_file_write_bytes(
        path.c_str(), INFILTRATR_ATOMIC_FILE_PRIVATE, value.data(), value.size());
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
