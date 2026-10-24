// SPDX-License-Identifier: GPL-3.0-or-later
#include "drag_drop_controller.hpp"
#include "clipboard_controller.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace infiltrator::files {
namespace {
GtkWidget *find_item(GtkWidget *view, double x, double y)
{
    GtkWidget *picked = gtk_widget_pick(view, x, y, GTK_PICK_DEFAULT);
    for (GtkWidget *node = picked; node != nullptr && node != view;
         node = gtk_widget_get_parent(node)) {
        if (g_object_get_data(G_OBJECT(node), "ifm-source-name") != nullptr) return node;
    }
    return nullptr;
}
GtkWidget *find_row(GtkWidget *view, double x, double y)
{
    GtkWidget *picked = gtk_widget_pick(view, x, y, GTK_PICK_DEFAULT);
    for (GtkWidget *node = picked; node != nullptr && node != view;
         node = gtk_widget_get_parent(node)) {
        if (GTK_IS_LIST_BOX_ROW(node)) return node;
    }
    return nullptr;
}
} // namespace

DragDropController::DragDropController(GtkWindow *window, GtkDirectoryList *directory,
                                       GtkMultiSelection *selection, GtkWidget *stack,
                                       GtkWidget *sidebar)
    : window_(window), directory_(directory), selection_(selection),
      stack_(stack), sidebar_(sidebar)
{
    if (window_ == nullptr || directory_ == nullptr || selection_ == nullptr ||
        stack_ == nullptr || sidebar_ == nullptr) return;
    g_object_ref(directory_);
    g_object_ref(selection_);
    for (const char *name : {"list", "icons", "compact"}) {
        GtkWidget *scroller = gtk_stack_get_child_by_name(GTK_STACK(stack_), name);
        if (GTK_IS_SCROLLED_WINDOW(scroller)) {
            GtkWidget *view = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scroller));
            if (view != nullptr) register_view(view);
        }
    }
    GtkWidget *secondary = GTK_WIDGET(
        g_object_get_data(G_OBJECT(window_), "ifm-secondary-pane-list"));
    if (GTK_IS_LIST_VIEW(secondary)) {
        GtkSelectionModel *model = gtk_list_view_get_model(GTK_LIST_VIEW(secondary));
        if (GTK_IS_MULTI_SELECTION(model)) {
            GListModel *directory = gtk_multi_selection_get_model(GTK_MULTI_SELECTION(model));
            if (GTK_IS_SORT_LIST_MODEL(directory))
                directory = gtk_sort_list_model_get_model(GTK_SORT_LIST_MODEL(directory));
            if (GTK_IS_DIRECTORY_LIST(directory)) {
                secondary_view_ = secondary;
                secondary_directory_ = GTK_DIRECTORY_LIST(g_object_ref(directory));
                secondary_selection_ = GTK_MULTI_SELECTION(g_object_ref(model));
                register_view(secondary_view_);
            }
        }
    }
    GtkDropTarget *sidebar_drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST,
        static_cast<GdkDragAction>(GDK_ACTION_COPY | GDK_ACTION_MOVE));
    g_signal_connect(sidebar_drop, "drop", G_CALLBACK(on_drop), this);
    g_signal_connect(sidebar_drop, "motion", G_CALLBACK(on_motion), this);
    gtk_widget_add_controller(sidebar_, GTK_EVENT_CONTROLLER(sidebar_drop));
    g_object_weak_ref(G_OBJECT(window_), on_window_finalized, this);
}
DragDropController::~DragDropController()
{
    if (directory_ != nullptr) g_object_unref(directory_);
    if (selection_ != nullptr) g_object_unref(selection_);
    if (secondary_selection_ != nullptr) g_object_unref(secondary_selection_);
    if (secondary_directory_ != nullptr) g_object_unref(secondary_directory_);
}
void DragDropController::on_window_finalized(gpointer data, GObject *)
{
    auto *self = static_cast<DragDropController *>(data);
    self->window_ = nullptr;
    delete self;
}
void DragDropController::register_view(GtkWidget *view)
{
    GtkDragSource *drag = gtk_drag_source_new();
    gtk_drag_source_set_actions(drag,
        static_cast<GdkDragAction>(GDK_ACTION_COPY | GDK_ACTION_MOVE));
    // The destination's operation engine owns source deletion, including
    // recovery. GTK must never delete drag data as a second operation.
    g_signal_connect(drag, "drag-end", G_CALLBACK(+[](GtkDragSource *,
        GdkDrag *, gboolean, gpointer) {}), nullptr);
    g_signal_connect(drag, "prepare", G_CALLBACK(on_prepare), this);
    gtk_widget_add_controller(view, GTK_EVENT_CONTROLLER(drag));
    GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST,
        static_cast<GdkDragAction>(GDK_ACTION_COPY | GDK_ACTION_MOVE));
    g_signal_connect(drop, "drop", G_CALLBACK(on_drop), this);
    g_signal_connect(drop, "motion", G_CALLBACK(on_motion), this);
    gtk_widget_add_controller(view, GTK_EVENT_CONTROLLER(drop));
}
std::vector<std::string> DragDropController::selected_paths(GtkWidget *view, double x, double y)
{
    std::vector<std::string> paths;
    GtkDirectoryList *directory = view == secondary_view_ ? secondary_directory_ : directory_;
    GtkMultiSelection *selection = view == secondary_view_ ? secondary_selection_ : selection_;
    if (directory == nullptr || selection == nullptr) return paths;
    GFile *parent = gtk_directory_list_get_file(directory);
    if (parent == nullptr || !g_file_is_native(parent)) return paths;
    GtkWidget *cell = find_item(view, x, y);
    if (cell == nullptr) return paths;
    const char *clicked = static_cast<const char *>(
        g_object_get_data(G_OBJECT(cell), "ifm-source-name"));
    if (clicked == nullptr) return paths;
    GListModel *model = gtk_multi_selection_get_model(selection);
    if (model == nullptr) return paths;
    guint position = GTK_INVALID_LIST_POSITION;
    for (guint i = 0; i < g_list_model_get_n_items(model); ++i) {
        auto *info = G_FILE_INFO(g_list_model_get_item(model, i));
        if (info == nullptr) continue;
        bool matches = g_strcmp0(g_file_info_get_name(info), clicked) == 0;
        g_object_unref(info);
        if (matches) { position = i; break; }
    }
    if (position == GTK_INVALID_LIST_POSITION) return paths;
    if (!gtk_selection_model_is_selected(GTK_SELECTION_MODEL(selection), position)) {
        gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(selection));
        gtk_selection_model_select_item(GTK_SELECTION_MODEL(selection), position, TRUE);
    }
    GtkBitset *selected = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(selection));
    if (gtk_bitset_get_size(selected) > 256U) {
        gtk_bitset_unref(selected);
        return paths;
    }
    GtkBitsetIter iter;
    guint i = 0;
    for (gboolean valid = gtk_bitset_iter_init_first(&iter, selected, &i);
         valid; valid = gtk_bitset_iter_next(&iter, &i)) {
        auto *info = G_FILE_INFO(g_list_model_get_item(model, i));
        if (info == nullptr) continue;
        const char *name = g_file_info_get_name(info);
        if (name != nullptr && *name != '\0') {
            GFile *file = g_file_get_child(parent, name);
            char *path = g_file_get_path(file);
            if (path != nullptr) paths.emplace_back(path);
            g_free(path);
            g_object_unref(file);
        }
        g_object_unref(info);
    }
    gtk_bitset_unref(selected);
    return paths;
}
GdkContentProvider *DragDropController::on_prepare(GtkDragSource *source, double x, double y,
                                                    gpointer data)
{
    auto *self = static_cast<DragDropController *>(data);
    GtkWidget *view = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
    if (view == nullptr) return nullptr;
    std::vector<std::string> paths = self->selected_paths(view, x, y);
    if (paths.empty()) return nullptr;
    GSList *files = nullptr;
    for (const std::string &path : paths) {
        files = g_slist_prepend(files, g_file_new_for_path(path.c_str()));
    }
    files = g_slist_reverse(files);
    GdkFileList *list = gdk_file_list_new_from_list(files);
    g_slist_free_full(files, reinterpret_cast<GDestroyNotify>(g_object_unref));
    if (list == nullptr) return nullptr;
    GdkContentProvider *provider = gdk_content_provider_new_typed(GDK_TYPE_FILE_LIST, list);
    g_boxed_free(GDK_TYPE_FILE_LIST, list);
    return provider;
}
GFile *DragDropController::target_folder(GtkWidget *target, double x, double y) const
{
    if (target == sidebar_) {
        GtkWidget *row = find_row(target, x, y);
        if (row == nullptr) return nullptr;
        const char *uri = static_cast<const char *>(
            g_object_get_data(G_OBJECT(row), "ifm-target"));
        return uri != nullptr ? g_file_new_for_commandline_arg(uri) : nullptr;
    }
    GtkDirectoryList *directory = target == secondary_view_ ? secondary_directory_ : directory_;
    GtkMultiSelection *selection = target == secondary_view_ ? secondary_selection_ : selection_;
    if (directory == nullptr || selection == nullptr) return nullptr;
    GFile *parent = gtk_directory_list_get_file(directory);
    if (parent == nullptr) return nullptr;
    GtkWidget *cell = find_item(target, x, y);
    const char *name = cell != nullptr ? static_cast<const char *>(
        g_object_get_data(G_OBJECT(cell), "ifm-source-name")) : nullptr;
    if (name != nullptr) {
        GListModel *model = gtk_multi_selection_get_model(selection);
        if (model != nullptr) {
            for (guint i = 0; i < g_list_model_get_n_items(model); ++i) {
                auto *info = G_FILE_INFO(g_list_model_get_item(model, i));
                if (info == nullptr) continue;
                const bool folder = g_strcmp0(g_file_info_get_name(info), name) == 0 &&
                                    g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
                g_object_unref(info);
                if (folder) return g_file_get_child(parent, name);
            }
        }
    }
    return G_FILE(g_object_ref(parent));
}
GdkDragAction DragDropController::on_motion(GtkDropTarget *target,
                                            double, double, gpointer)
{
    GdkDrop *drop = gtk_drop_target_get_current_drop(target);
    const bool local_drag = drop != nullptr && gdk_drop_get_drag(drop) != nullptr;
    const bool supports_move = drop != nullptr &&
        (gdk_drop_get_actions(drop) & GDK_ACTION_MOVE) != 0;
    const bool shift = (gtk_event_controller_get_current_event_state(
        GTK_EVENT_CONTROLLER(target)) & GDK_SHIFT_MASK) != 0;
    return local_drag && supports_move && shift ? GDK_ACTION_MOVE : GDK_ACTION_COPY;
}

gboolean DragDropController::on_drop(GtkDropTarget *target, const GValue *value,
                                     double x, double y, gpointer data)
{
    auto *self = static_cast<DragDropController *>(data);
    if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST) || self->window_ == nullptr)
        return FALSE;
    auto *operations = static_cast<ClipboardController *>(
        g_object_get_data(G_OBJECT(self->window_), "ifm-clipboard-controller"));
    if (operations == nullptr) return FALSE;
    GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
    GFile *folder = self->target_folder(widget, x, y);
    if (folder == nullptr) return FALSE;
    if (!g_file_is_native(folder) ||
        g_file_query_file_type(folder, G_FILE_QUERY_INFO_NONE, nullptr) !=
            G_FILE_TYPE_DIRECTORY) {
        g_object_unref(folder);
        return FALSE;
    }
    char *destination = g_file_get_path(folder);
    g_object_unref(folder);
    if (destination == nullptr) return FALSE;
    std::vector<std::string> sources;
    GSList *files = gdk_file_list_get_files(
        static_cast<GdkFileList *>(g_value_get_boxed(value)));
    // Do not silently truncate a drag batch. A partial accepted drop would
    // misrepresent both user intent and the recovery journal's input set.
    if (g_slist_length(files) > 256U) {
        g_slist_free(files);
        g_free(destination);
        return FALSE;
    }
    for (GSList *it = files; it != nullptr; it = it->next) {
        auto *file = G_FILE(it->data);
        if (file == nullptr || !g_file_is_native(file)) {
            g_slist_free(files);
            g_free(destination);
            return FALSE;
        }
        char *path = g_file_get_path(file);
        if (path == nullptr) {
            g_slist_free(files);
            g_free(destination);
            return FALSE;
        }
        sources.emplace_back(path);
        g_free(path);
    }
    g_slist_free(files);
    // A move is legal only for an unambiguously negotiated same-process drag.
    // Cross-application and ambiguous drops remain copy-only, preventing an
    // external drag source from deleting the originals after Files moves them.
    GdkDrop *drop = gtk_drop_target_get_current_drop(target);
    const bool shift = (gtk_event_controller_get_current_event_state(
        GTK_EVENT_CONTROLLER(target)) & GDK_SHIFT_MASK) != 0;
    const bool move = drop != nullptr && shift &&
        (gdk_drop_get_actions(drop) & GDK_ACTION_MOVE) != 0 &&
        gdk_drop_get_drag(drop) != nullptr;
    const bool accepted = operations->transfer_from_drop(sources, destination, move);
    g_free(destination);
    return accepted ? TRUE : FALSE;
}
} // namespace infiltrator::files
