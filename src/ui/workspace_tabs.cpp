// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_manager_window.hpp"
#include "theme_controller.hpp"

#include <algorithm>

namespace infiltrator::files {
namespace {
struct TabDrag {
    GWeakRef source;
    std::uint64_t id;
    TabDrag(GtkWindow *window, std::uint64_t value) : id(value) {
        g_weak_ref_init(&source, G_OBJECT(window));
    }
    TabDrag(const TabDrag &other) : id(other.id) {
        GObject *window = G_OBJECT(g_weak_ref_get(const_cast<GWeakRef *>(&other.source)));
        g_weak_ref_init(&source, window);
        g_clear_object(&window);
    }
    ~TabDrag() { g_weak_ref_clear(&source); }
};
}

std::uint64_t FileManagerWindow::next_tab_id() noexcept
{
    static std::uint64_t next = 0U; // All tab changes run on the GTK thread.
    return ++next;
}

GType FileManagerWindow::tab_drag_type()
{
    static const GType type = g_boxed_type_register_static("InfiltratorFilesTabDrag",
        +[](gpointer data) -> gpointer { return new TabDrag(*static_cast<TabDrag *>(data)); },
        +[](gpointer data) { delete static_cast<TabDrag *>(data); });
    return type;
}

void FileManagerWindow::install_tab_drag(GtkWidget *button, const std::uint64_t id)
{
    auto *identity = new std::uint64_t(id);
    g_object_set_data_full(G_OBJECT(button), "ifm-tab-id", identity,
                          +[](gpointer data) { delete static_cast<std::uint64_t *>(data); });
    auto *drag = gtk_drag_source_new();
    gtk_drag_source_set_actions(drag, GDK_ACTION_MOVE);
    g_signal_connect(drag, "prepare", G_CALLBACK(+[](GtkDragSource *source, double, double,
                                                     gpointer data) -> GdkContentProvider * {
        auto *self = static_cast<FileManagerWindow *>(data);
        auto *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
        const auto *value = static_cast<const std::uint64_t *>(
            g_object_get_data(G_OBJECT(widget), "ifm-tab-id"));
        if (value == nullptr || gtk_window_get_application(self->native_window()) == nullptr)
            return nullptr;
        TabDrag payload(self->native_window(), *value);
        return gdk_content_provider_new_typed(tab_drag_type(), &payload);
    }), this);
    gtk_widget_add_controller(button, GTK_EVENT_CONTROLLER(drag));
}

void FileManagerWindow::install_tab_drop()
{
    auto *drop = gtk_drop_target_new(tab_drag_type(), GDK_ACTION_MOVE);
    g_signal_connect(drop, "drop", G_CALLBACK(+[](GtkDropTarget *, const GValue *value,
                                                  double x, double y, gpointer data) -> gboolean {
        auto *destination = static_cast<FileManagerWindow *>(data);
        if (!G_VALUE_HOLDS(value, tab_drag_type())) return FALSE;
        auto *payload = static_cast<TabDrag *>(g_value_get_boxed(value));
        if (payload == nullptr) return FALSE;
        auto *window = GTK_WINDOW(g_weak_ref_get(&payload->source));
        auto *source = window != nullptr ? static_cast<FileManagerWindow *>(
            g_object_get_data(G_OBJECT(window), "ifm-window-controller")) : nullptr;
        bool moved = false;
        if (source == destination && gtk_window_get_application(window) != nullptr) {
            // A stable identity prevents a stale drag from moving another tab
            // after a close, reopen, or concurrent reorder.
            auto &tabs = source->tabs_;
            const auto from = std::find_if(tabs.begin(), tabs.end(), [&](const TabState &tab) {
                return tab.id == payload->id;
            });
            if (from != tabs.end()) {
                std::size_t target = tabs.size() - 1U;
                for (GtkWidget *picked = gtk_widget_pick(source->tab_strip_, x, y, GTK_PICK_DEFAULT);
                     picked != nullptr && picked != source->tab_strip_;
                     picked = gtk_widget_get_parent(picked)) {
                    const int encoded = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(picked), "ifm-tab-index"));
                    if (encoded > 0) { target = static_cast<std::size_t>(encoded - 1); break; }
                }
                source->remember_tab();
                const auto active_id = tabs[source->active_tab_].id;
                TabState tab = std::move(*from);
                tabs.erase(from);
                tabs.insert(tabs.begin() + static_cast<std::ptrdiff_t>(target), std::move(tab));
                const auto active = std::find_if(tabs.begin(), tabs.end(), [&](const TabState &item) {
                    return item.id == active_id;
                });
                source->active_tab_ = static_cast<std::size_t>(active - tabs.begin());
                source->update_tab_buttons();
                moved = true;
            }
        } else if (source != nullptr) {
            moved = source->move_tab_to(*destination, payload->id);
        }
        g_clear_object(&window);
        return moved;
    }), this);
    gtk_widget_add_controller(tab_strip_, GTK_EVENT_CONTROLLER(drop));
}

bool FileManagerWindow::move_tab_to(FileManagerWindow &destination, const std::uint64_t id)
{
    auto *app = gtk_window_get_application(native_window());
    if (&destination == this || app == nullptr ||
        gtk_window_get_application(destination.native_window()) != app ||
        destination.tabs_.size() >= 128U) return false;
    remember_tab();
    const auto found = std::find_if(tabs_.begin(), tabs_.end(), [id](const TabState &tab) {
        return tab.id == id;
    });
    if (found == tabs_.end()) return false;
    const auto index = static_cast<std::size_t>(found - tabs_.begin());
    destination.remember_tab();
    destination.tabs_.push_back(*found);
    destination.active_tab_ = destination.tabs_.size() - 1U;
    destination.display_active_tab();
    remove_tab(index, false); // Destroy an empty source only after acceptance.
    return true;
}

bool FileManagerWindow::move_active_tab_to(FileManagerWindow &destination)
{
    return active_tab_ < tabs_.size() && move_tab_to(destination, tabs_[active_tab_].id);
}

void FileManagerWindow::move_active_tab_to_new_window()
{
    auto *app = gtk_window_get_application(native_window());
    if (app == nullptr || active_tab_ >= tabs_.size()) return;
    auto *destination = new FileManagerWindow(app);
    destination->install_operation_surface();
    new ThemeController(destination->native_window());
    // Replace the fresh window's temporary root tab with the accepted tab.
    destination->tabs_.clear();
    destination->active_tab_ = 0U;
    if (move_active_tab_to(*destination)) destination->present();
    else gtk_window_destroy(destination->native_window());
}
} // namespace infiltrator::files
