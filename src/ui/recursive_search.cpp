// SPDX-License-Identifier: GPL-3.0-or-later
#include "recursive_search.hpp"

#include "file_manager_window.hpp"
#include "../core/search_engine.hpp"

#include <charconv>
#include <cstring>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace infiltrator::files {
namespace {

struct SearchWindow {
    GFile *root{nullptr};
    GtkWindow *owner{nullptr}; // Lifetime bounded by destroy-with-parent
    GtkWidget *name{nullptr};
    GtkWidget *mode{nullptr};
    GtkWidget *case_sensitive{nullptr};
    GtkWidget *recursive{nullptr};
    GtkWidget *include_hidden{nullptr};
    GtkWidget *kind{nullptr};
    GtkWidget *minimum{nullptr};
    GtkWidget *maximum{nullptr};
    GtkWidget *since{nullptr};
    GtkWidget *content{nullptr};
    GtkWidget *status{nullptr};
    GtkWidget *results{nullptr};
    GtkWidget *search{nullptr};
    GtkWidget *cancel{nullptr};
    GtkWidget *reveal{nullptr};
    GCancellable *active{nullptr};
    ~SearchWindow() {
        if (active != nullptr) {
            g_cancellable_cancel(active);
            g_object_unref(active);
        }
        if (root != nullptr) g_object_unref(root);
    }
};

struct SearchWork {
    GFile *root;
    SearchOptions options;
    SearchWork(GFile *location, SearchOptions query)
        : root(G_FILE(g_object_ref(location))), options(std::move(query)) {}
    ~SearchWork() { g_object_unref(root); }
};

SearchWindow *state(GtkWidget *dialog)
{
    return static_cast<SearchWindow *>(
        g_object_get_data(G_OBJECT(dialog), "ifm-recursive-search-state"));
}

GtkWidget *field(GtkGrid *grid, const char *label, int column, int row,
                 const char *placeholder)
{
    GtkWidget *group = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_hexpand(group, TRUE);
    GtkWidget *heading = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(heading), 0);
    gtk_widget_add_css_class(heading, "dim-label");
    gtk_box_append(GTK_BOX(group), heading);
    GtkWidget *entry = gtk_entry_new();
    if (placeholder != nullptr) gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
    gtk_box_append(GTK_BOX(group), entry);
    gtk_grid_attach(grid, group, column, row, 1, 1);
    return entry;
}

GtkWidget *choice(GtkGrid *grid, const char *label, int column, int row,
                  const char *const *strings)
{
    GtkWidget *group = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_hexpand(group, TRUE);
    GtkWidget *heading = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(heading), 0);
    gtk_widget_add_css_class(heading, "dim-label");
    gtk_box_append(GTK_BOX(group), heading);
    GtkWidget *drop = gtk_drop_down_new_from_strings(strings);
    gtk_box_append(GTK_BOX(group), drop);
    gtk_grid_attach(grid, group, column, row, 1, 1);
    return drop;
}

std::optional<std::uint64_t> bytes(GtkWidget *entry, bool &valid)
{
    const char *value = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (value == nullptr || value[0] == '\0') return std::nullopt;
    std::uint64_t result = 0;
    const char *end = value + std::strlen(value);
    const auto parsed = std::from_chars(value, end, result, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        valid = false;
        return std::nullopt;
    }
    return result;
}

std::optional<std::int64_t> date(GtkWidget *entry, bool &valid)
{
    const char *value = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (value == nullptr || value[0] == '\0') return std::nullopt;
    if (std::strlen(value) != 10 || value[4] != '-' || value[7] != '-') {
        valid = false;
        return std::nullopt;
    }
    for (int index = 0; index < 10; ++index) {
        if (index == 4 || index == 7) continue;
        if (!g_ascii_isdigit(value[index])) {
            valid = false;
            return std::nullopt;
        }
    }
    const auto number = [&](int start, int length) {
        int out = 0;
        for (int i = 0; i < length; ++i) out = out * 10 + value[start + i] - '0';
        return out;
    };
    GDateTime *time = g_date_time_new_local(number(0, 4), number(5, 2),
                                           number(8, 2), 0, 0, 0);
    if (time == nullptr) {
        valid = false;
        return std::nullopt;
    }
    const std::int64_t seconds = g_date_time_to_unix(time);
    g_date_time_unref(time);
    return seconds;
}

GtkListBoxRow *selected(GtkWidget *dialog)
{
    SearchWindow *s = state(dialog);
    return s != nullptr ? gtk_list_box_get_selected_row(GTK_LIST_BOX(s->results)) : nullptr;
}

void reveal(GtkWidget *dialog)
{
    SearchWindow *s = state(dialog);
    if (s == nullptr || s->owner == nullptr) return;
    GtkListBoxRow *row = selected(dialog);
    if (row == nullptr) return;
    GFile *file = G_FILE(g_object_get_data(G_OBJECT(row), "ifm-search-result-file"));
    if (file == nullptr) return;
    auto *window = static_cast<FileManagerWindow *>(
        g_object_get_data(G_OBJECT(s->owner), "ifm-window-controller"));
    if (window != nullptr) window->reveal_search_result(file);
    gtk_window_destroy(GTK_WINDOW(dialog));
}

void run_search(GTask *task, gpointer, gpointer data, GCancellable *cancel)
{
    auto *work = static_cast<SearchWork *>(data);
    GError *error = nullptr;
    SearchReport result = search_files(work->root, work->options, cancel, &error);
    if (error != nullptr) {
        g_task_return_error(task, error);
    } else {
        g_task_return_pointer(task, new SearchReport(std::move(result)),
                              [](gpointer ptr) { delete static_cast<SearchReport *>(ptr); });
    }
}

void completed(GObject *, GAsyncResult *result, gpointer data)
{
    std::unique_ptr<GWeakRef, void(*)(GWeakRef *)> weak(
        static_cast<GWeakRef *>(data),
        [](GWeakRef *ptr) { g_weak_ref_clear(ptr); delete ptr; });
    GError *error = nullptr;
    std::unique_ptr<SearchReport> report(static_cast<SearchReport *>(
        g_task_propagate_pointer(G_TASK(result), &error)));
    GtkWidget *dialog = GTK_WIDGET(g_weak_ref_get(weak.get()));
    if (dialog != nullptr) {
        SearchWindow *s = state(dialog);
        if (s != nullptr) {
            g_clear_object(&s->active);
            gtk_widget_set_sensitive(s->search, TRUE);
            gtk_widget_set_sensitive(s->cancel, FALSE);
            if (error != nullptr) {
                gtk_label_set_text(GTK_LABEL(s->status), error->message);
            } else if (report != nullptr) {
                for (const SearchHit &hit : report->hits) {
                    GtkWidget *row = gtk_list_box_row_new();
                    GtkWidget *details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
                    gtk_widget_set_margin_start(details, 10);
                    gtk_widget_set_margin_end(details, 10);
                    gtk_widget_set_margin_top(details, 6);
                    gtk_widget_set_margin_bottom(details, 6);
                    GtkWidget *name = gtk_label_new(hit.name.c_str());
                    gtk_label_set_xalign(GTK_LABEL(name), 0);
                    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
                    gtk_box_append(GTK_BOX(details), name);
                    GtkWidget *path = gtk_label_new(hit.location.c_str());
                    gtk_label_set_xalign(GTK_LABEL(path), 0);
                    gtk_label_set_ellipsize(GTK_LABEL(path), PANGO_ELLIPSIZE_MIDDLE);
                    gtk_widget_add_css_class(path, "dim-label");
                    gtk_box_append(GTK_BOX(details), path);
                    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), details);
                    g_object_set_data_full(G_OBJECT(row), "ifm-search-result-file",
                        g_file_new_for_uri(hit.uri.c_str()), g_object_unref);
                    gtk_list_box_append(GTK_LIST_BOX(s->results), row);
                }
                std::string message = std::to_string(report->hits.size()) + " matches across " +
                    std::to_string(report->visited) + " entries";
                if (!report->complete) message += " — incomplete: " + report->limitation;
                if (report->content_skipped != 0)
                    message += " — content not searched in " +
                        std::to_string(report->content_skipped) + " files";
                if (report->complete && report->content_skipped == 0) message += " — complete";
                gtk_label_set_text(GTK_LABEL(s->status), message.c_str());
            }
        }
        g_object_unref(dialog);
    }
    g_clear_error(&error);
}

void start(GtkButton *, gpointer data)
{
    GtkWidget *dialog = GTK_WIDGET(data);
    SearchWindow *s = state(dialog);
    if (s == nullptr || s->active != nullptr) return;
    SearchOptions o;
    o.name = gtk_editable_get_text(GTK_EDITABLE(s->name));
    o.content = gtk_editable_get_text(GTK_EDITABLE(s->content));
    o.case_sensitive = gtk_check_button_get_active(GTK_CHECK_BUTTON(s->case_sensitive));
    o.recursive = gtk_check_button_get_active(GTK_CHECK_BUTTON(s->recursive));
    o.include_hidden = gtk_check_button_get_active(GTK_CHECK_BUTTON(s->include_hidden));
    o.name_mode = static_cast<SearchNameMode>(
        gtk_drop_down_get_selected(GTK_DROP_DOWN(s->mode)));
    o.kind = static_cast<SearchItemKind>(
        gtk_drop_down_get_selected(GTK_DROP_DOWN(s->kind)));
    bool valid = true;
    o.minimum_size = bytes(s->minimum, valid);
    o.maximum_size = bytes(s->maximum, valid);
    o.modified_since = date(s->since, valid);
    if (o.minimum_size && o.maximum_size && *o.minimum_size > *o.maximum_size)
        valid = false;
    if (!valid) {
        gtk_label_set_text(GTK_LABEL(s->status),
            "Check date (YYYY-MM-DD) and integer byte limits; minimum must not exceed maximum.");
        return;
    }
    if (o.name.empty() && o.content.empty() && o.kind == SearchItemKind::Any &&
        !o.minimum_size && !o.maximum_size && !o.modified_since) {
        gtk_label_set_text(GTK_LABEL(s->status),
            "Enter a name, text or filter to start a bounded search.");
        return;
    }
    if (!g_utf8_validate(o.name.c_str(), -1, nullptr) ||
        !g_utf8_validate(o.content.c_str(), -1, nullptr)) {
        gtk_label_set_text(GTK_LABEL(s->status), "Search text must be valid UTF-8.");
        return;
    }

    GtkWidget *row = gtk_widget_get_first_child(s->results);
    while (row != nullptr) {
        gtk_list_box_remove(GTK_LIST_BOX(s->results), row);
        row = gtk_widget_get_first_child(s->results);
    }
    gtk_widget_set_sensitive(s->reveal, FALSE);
    gtk_label_set_text(GTK_LABEL(s->status), "Searching…");
    gtk_widget_set_sensitive(s->search, FALSE);
    gtk_widget_set_sensitive(s->cancel, TRUE);
    s->active = g_cancellable_new();
    auto *weak = new GWeakRef;
    g_weak_ref_init(weak, G_OBJECT(dialog));
    GTask *task = g_task_new(nullptr, s->active, completed, weak);
    g_task_set_task_data(task, new SearchWork(s->root, std::move(o)),
        [](gpointer ptr) { delete static_cast<SearchWork *>(ptr); });
    g_task_run_in_thread(task, run_search);
    g_object_unref(task);
}

} // namespace

void show_recursive_search(GtkWindow *owner, GFile *location)
{
    if (owner == nullptr || location == nullptr) return;
    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Search files");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), owner);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_modal(GTK_WINDOW(dialog), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 760, 690);
    auto *s = new SearchWindow;
    s->owner = owner;
    s->root = G_FILE(g_object_ref(location));
    g_object_set_data_full(G_OBJECT(dialog), "ifm-recursive-search-state", s,
        [](gpointer ptr) { delete static_cast<SearchWindow *>(ptr); });

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    gtk_widget_set_margin_start(outer, 14);
    gtk_widget_set_margin_end(outer, 14);
    gtk_widget_set_margin_top(outer, 14);
    gtk_widget_set_margin_bottom(outer, 14);
    gtk_window_set_child(GTK_WINDOW(dialog), outer);
    char *location_name = g_file_get_parse_name(location);
    const std::string scope = "Search within: " + std::string(
        location_name != nullptr ? location_name : "current folder");
    g_free(location_name);
    GtkWidget *intro = gtk_label_new(scope.c_str());
    gtk_label_set_xalign(GTK_LABEL(intro), 0);
    gtk_label_set_ellipsize(GTK_LABEL(intro), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(outer), intro);

    GtkWidget *options = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(options), 14);
    gtk_grid_set_row_spacing(GTK_GRID(options), 8);
    gtk_box_append(GTK_BOX(outer), options);
    s->name = field(GTK_GRID(options), "Filename", 0, 0, "Part of name, exact name or *.pdf");
    constexpr const char *modes[] = {"Contains", "Exact", "Wildcard (* and ?)", nullptr};
    s->mode = choice(GTK_GRID(options), "Name matching", 1, 0, modes);
    s->content = field(GTK_GRID(options), "Text within file (1 MiB limit)", 0, 1, "Optional UTF-8 text");
    constexpr const char *kinds[] = {"Files and folders", "Files only", "Folders only", nullptr};
    s->kind = choice(GTK_GRID(options), "Type", 1, 1, kinds);
    s->minimum = field(GTK_GRID(options), "Minimum size in bytes", 0, 2, "Optional");
    s->maximum = field(GTK_GRID(options), "Maximum size in bytes", 1, 2, "Optional");
    s->since = field(GTK_GRID(options), "Modified on/after (local YYYY-MM-DD)", 0, 3, "Optional");
    GtkWidget *flags = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    s->recursive = gtk_check_button_new_with_label("Include subfolders");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(s->recursive), TRUE);
    s->case_sensitive = gtk_check_button_new_with_label("Case-sensitive text and names");
    s->include_hidden = gtk_check_button_new_with_label("Include hidden items");
    gtk_box_append(GTK_BOX(flags), s->recursive);
    gtk_box_append(GTK_BOX(flags), s->case_sensitive);
    gtk_box_append(GTK_BOX(flags), s->include_hidden);
    gtk_grid_attach(GTK_GRID(options), flags, 1, 3, 1, 1);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(outer), toolbar);
    s->search = gtk_button_new_with_label("Search");
    gtk_widget_add_css_class(s->search, "suggested-action");
    s->cancel = gtk_button_new_with_label("Cancel scan");
    gtk_widget_set_sensitive(s->cancel, FALSE);
    gtk_box_append(GTK_BOX(toolbar), s->search);
    gtk_box_append(GTK_BOX(toolbar), s->cancel);
    s->status = gtk_label_new("Ready — read-only search, up to 500 matches and 100,000 entries");
    gtk_label_set_xalign(GTK_LABEL(s->status), 0);
    gtk_label_set_wrap(GTK_LABEL(s->status), TRUE);
    gtk_box_append(GTK_BOX(outer), s->status);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    s->results = gtk_list_box_new();
    g_object_set_data(G_OBJECT(dialog), "ifm-recursive-results", s->results);
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(s->results), GTK_SELECTION_SINGLE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), s->results);
    gtk_box_append(GTK_BOX(outer), scroll);
    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(outer), footer);
    s->reveal = gtk_button_new_with_label("Reveal in containing folder");
    gtk_widget_set_sensitive(s->reveal, FALSE);
    gtk_box_append(GTK_BOX(footer), s->reveal);
    GtkWidget *close = gtk_button_new_with_label("Close");
    gtk_widget_set_hexpand(close, TRUE);
    gtk_widget_set_halign(close, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(footer), close);
    g_signal_connect(s->search, "clicked", G_CALLBACK(start), dialog);
    g_signal_connect(s->cancel, "clicked", G_CALLBACK((+[](GtkButton *, gpointer data) {
        SearchWindow *state = static_cast<SearchWindow *>(data);
        if (state->active != nullptr) g_cancellable_cancel(state->active);
    })), s);
    g_signal_connect(s->results, "row-selected", G_CALLBACK((+[](GtkListBox *, GtkListBoxRow *row, gpointer data) {
        auto *state = static_cast<SearchWindow *>(data);
        gtk_widget_set_sensitive(state->reveal, row != nullptr);
    })), s);
    g_signal_connect(s->results, "row-activated", G_CALLBACK((+[](GtkListBox *, GtkListBoxRow *, gpointer data) {
        reveal(GTK_WIDGET(data));
    })), dialog);
    g_signal_connect(s->reveal, "clicked", G_CALLBACK((+[](GtkButton *, gpointer data) {
        reveal(GTK_WIDGET(data));
    })), dialog);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_destroy), dialog);
    gtk_window_present(GTK_WINDOW(dialog));
    gtk_widget_grab_focus(s->name);
}

} // namespace infiltrator::files
