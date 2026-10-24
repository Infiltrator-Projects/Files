// SPDX-License-Identifier: GPL-3.0-or-later
#include "recursive_search.hpp"

#include "file_manager_window.hpp"
#include "../core/search_engine.hpp"
#include "tag_catalogue.hpp"
#include "search_persistence.hpp"

#include <charconv>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <memory>
#include <iterator>
#include <vector>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace infiltrator::files {
namespace {

struct SearchWindow {
    GFile *root{nullptr};
    std::vector<GFile *> selected_roots;
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
    GtkWidget *before{nullptr};
    GtkWidget *owner_uid{nullptr};
    GtkWidget *tag{nullptr};
    GtkWidget *excluded{nullptr};
    GtkWidget *duplicates{nullptr};
    GtkWidget *content{nullptr};
    GtkWidget *status{nullptr};
    GtkWidget *results{nullptr};
    GtkWidget *search{nullptr};
    GtkWidget *cancel{nullptr};
    GtkWidget *reveal{nullptr};
    GtkWidget *scope_label{nullptr};
    GtkWidget *preset_name{nullptr};
    GtkWidget *preset_choice{nullptr};
    GtkWidget *preset_save{nullptr};
    GtkWidget *privacy_save{nullptr};
    std::optional<SearchOptions> last_options;
    GCancellable *active{nullptr};
    ~SearchWindow() {
        if (active != nullptr) {
            g_cancellable_cancel(active);
            g_object_unref(active);
        }
        if (root != nullptr) g_object_unref(root);
        for (GFile *folder : selected_roots) g_object_unref(folder);
    }
};

struct SearchWork {
    std::vector<GFile *> roots;
    SearchOptions options;
    SearchWork(const std::vector<GFile *> &locations, SearchOptions query)
        : options(std::move(query)) {
        for (GFile *location : locations) roots.push_back(G_FILE(g_object_ref(location)));
    }
    ~SearchWork() { for (GFile *root : roots) g_object_unref(root); }
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
    SearchReport result;
    for (std::size_t index = 0; index < work->roots.size(); ++index) {
        if (g_cancellable_set_error_if_cancelled(cancel, &error)) break;
        SearchReport next = search_files(work->roots[index], work->options, cancel, &error);
        if (error != nullptr) break;
        result.visited += next.visited;
        result.content_skipped += next.content_skipped;
        result.duplicate_skipped += next.duplicate_skipped;
        result.hits.insert(result.hits.end(),
            std::make_move_iterator(next.hits.begin()),
            std::make_move_iterator(next.hits.end()));
        if (!next.complete) {
            result.complete = false;
            if (result.limitation.empty()) result.limitation = next.limitation;
        }
        if (result.hits.size() >= 500 || result.visited >= 100000) {
            result.complete = false;
            result.limitation = "Combined search reached its 500-result/100,000-entry bound";
            break;
        }
    }
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
                if (report->duplicate_skipped != 0)
                    message += " — duplicates not checked in " +
                        std::to_string(report->duplicate_skipped) + " files";
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
    o.modified_before = date(s->before, valid);
    const auto owner = bytes(s->owner_uid, valid);
    if (owner && *owner > G_MAXUINT32) valid = false;
    else if (owner) o.owner_uid = static_cast<std::uint32_t>(*owner);
    o.duplicates_by_sha256 =
        gtk_check_button_get_active(GTK_CHECK_BUTTON(s->duplicates));
    const char *tag = gtk_editable_get_text(GTK_EDITABLE(s->tag));
    if (tag != nullptr && *tag != '\0') {
        std::string name(tag);
        if (!valid_tag(name)) valid = false;
        else {
            const TagCatalogue catalogue = load_tag_catalogue();
            if (!catalogue.writable) {
                gtk_label_set_text(GTK_LABEL(s->status), catalogue.error.c_str());
                return;
            }
            o.restrict_tagged_uris = true;
            o.tagged_uris = uris_for_tag(catalogue, name);
        }
    }
    const char *excluded = gtk_editable_get_text(GTK_EDITABLE(s->excluded));
    if (excluded != nullptr && *excluded != '\0') {
        // Per-query exclusions are absolute URIs/paths, not wildcards. A path
        // is converted to GFile once, then compared by GIO identity.
        gchar **parts = g_strsplit(excluded, ";", 65);
        for (gchar **p = parts; p != nullptr && *p != nullptr; ++p) {
            char *trim = g_strstrip(*p);
            if (*trim == '\0') continue;
            if (!(g_str_has_prefix(trim, "/") || g_str_has_prefix(trim, "file://") ||
                  strstr(trim, "://") != nullptr)) {
                valid = false;
                break;
            }
            GFile *file = strstr(trim, "://") != nullptr ?
                g_file_new_for_uri(trim) : g_file_new_for_path(trim);
            char *uri = g_file_get_uri(file);
            if (uri != nullptr) o.excluded_uris.emplace_back(uri);
            g_free(uri);
            g_object_unref(file);
        }
        g_strfreev(parts);
    }
    if (o.modified_since && o.modified_before &&
        *o.modified_since >= *o.modified_before) valid = false;
    if (o.minimum_size && o.maximum_size && *o.minimum_size > *o.maximum_size)
        valid = false;
    if (!valid) {
        gtk_label_set_text(GTK_LABEL(s->status),
            "Check date ranges, UID, excluded absolute locations and byte bounds.");
        return;
    }
    if (o.name.empty() && o.content.empty() && o.kind == SearchItemKind::Any &&
        !o.minimum_size && !o.maximum_size && !o.modified_since &&
        !o.modified_before && !o.owner_uid && !o.duplicates_by_sha256 &&
        !o.restrict_tagged_uris) {
        gtk_label_set_text(GTK_LABEL(s->status),
            "Enter a name, text or filter to start a bounded search.");
        return;
    }
    const auto privacy = load_search_privacy();
    if (!privacy.valid) {
        gtk_label_set_text(GTK_LABEL(s->status), privacy.error.c_str());
        return;
    }
    for (const std::string &uri : privacy.excluded_uris) {
        if (std::find(o.excluded_uris.begin(), o.excluded_uris.end(), uri) ==
            o.excluded_uris.end())
            o.excluded_uris.push_back(uri);
    }
    if (o.excluded_uris.size() > 64) {
        gtk_label_set_text(GTK_LABEL(s->status), "Combined exclusions exceed 64 locations.");
        return;
    }
    if (!g_utf8_validate(o.name.c_str(), -1, nullptr) ||
        !g_utf8_validate(o.content.c_str(), -1, nullptr)) {
        gtk_label_set_text(GTK_LABEL(s->status), "Search text must be valid UTF-8.");
        return;
    }

    s->last_options = o;
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
    std::vector<GFile *> roots = s->selected_roots;
    if (roots.empty()) roots.push_back(s->root);
    g_task_set_task_data(task, new SearchWork(roots, std::move(o)),
        [](gpointer ptr) { delete static_cast<SearchWork *>(ptr); });
    g_task_run_in_thread(task, run_search);
    g_object_unref(task);
}

} // namespace


void refresh_presets(SearchWindow *s)
{
    if (s == nullptr || s->preset_choice == nullptr) return;
    const SavedSearchList presets = load_saved_searches();
    if (!presets.writable) {
        gtk_label_set_text(GTK_LABEL(s->status), presets.error.c_str());
        gtk_widget_set_sensitive(s->preset_save, FALSE);
        return;
    }
    GtkStringList *names = gtk_string_list_new(nullptr);
    for (const SavedSearch &item : presets.items)
        gtk_string_list_append(names, item.title.c_str());
    gtk_drop_down_set_model(GTK_DROP_DOWN(s->preset_choice), G_LIST_MODEL(names));
    g_object_unref(names);
}

void save_preset(GtkButton *, gpointer data)
{
    auto *s = state(GTK_WIDGET(data));
    if (s == nullptr || !s->last_options || !s->selected_roots.empty()) {
        if (s != nullptr)
            gtk_label_set_text(GTK_LABEL(s->status),
                "Run a current-folder search before saving (multi-folder presets not supported).");
        return;
    }
    const char *raw_name = gtk_editable_get_text(GTK_EDITABLE(s->preset_name));
    const std::string title = raw_name != nullptr ? raw_name : "";
    if (title.empty() || title.size() > 80 ||
        !g_utf8_validate(title.c_str(), -1, nullptr)) {
        gtk_label_set_text(GTK_LABEL(s->status), "Preset names must contain 1–80 UTF-8 bytes.");
        return;
    }
    SavedSearchList saved = load_saved_searches();
    if (!saved.writable) {
        gtk_label_set_text(GTK_LABEL(s->status), saved.error.c_str());
        return;
    }
    SavedSearch entry;
    entry.title = title;
    char *uri = g_file_get_uri(s->root);
    entry.root_uri = uri != nullptr ? uri : "";
    g_free(uri);
    entry.options = *s->last_options;
    // Privacy exclusions are independently owned, so a saved preset cannot
    // accidentally override or replace their canonical policy.
    entry.options.excluded_uris.clear();
    entry.options.tagged_uris.clear();
    entry.options.restrict_tagged_uris = false;
    const char *tag = gtk_editable_get_text(GTK_EDITABLE(s->tag));
    entry.tag = tag != nullptr ? tag : "";
    auto it = std::find_if(saved.items.begin(), saved.items.end(),
        [&](const SavedSearch &candidate) { return candidate.title == title; });
    if (it != saved.items.end()) *it = std::move(entry);
    else if (saved.items.size() < 16) saved.items.push_back(std::move(entry));
    else {
        gtk_label_set_text(GTK_LABEL(s->status), "Saved-search limit: 16 named presets.");
        return;
    }
    std::string error;
    if (!save_saved_searches(saved, error)) {
        gtk_label_set_text(GTK_LABEL(s->status), error.c_str());
        return;
    }
    refresh_presets(s);
    gtk_label_set_text(GTK_LABEL(s->status), "Named search saved atomically.");
}

void set_date_field(GtkWidget *entry, std::optional<std::int64_t> seconds)
{
    if (!seconds) { gtk_editable_set_text(GTK_EDITABLE(entry), ""); return; }
    GDateTime *time = g_date_time_new_from_unix_local(*seconds);
    if (time == nullptr) { gtk_editable_set_text(GTK_EDITABLE(entry), ""); return; }
    char *value = g_date_time_format(time, "%Y-%m-%d");
    gtk_editable_set_text(GTK_EDITABLE(entry), value != nullptr ? value : "");
    g_free(value);
    g_date_time_unref(time);
}

void load_preset(GtkButton *, gpointer data)
{
    auto *s = state(GTK_WIDGET(data));
    if (s == nullptr) return;
    const auto saved = load_saved_searches();
    if (!saved.writable) {
        gtk_label_set_text(GTK_LABEL(s->status), saved.error.c_str());
        return;
    }
    const guint index = gtk_drop_down_get_selected(GTK_DROP_DOWN(s->preset_choice));
    if (index >= saved.items.size()) return;
    const SavedSearch &p = saved.items[index];
    GFile *folder = g_file_new_for_uri(p.root_uri.c_str());
    if (s->root != nullptr) g_object_unref(s->root);
    s->root = folder;
    for (GFile *selected : s->selected_roots) g_object_unref(selected);
    s->selected_roots.clear();
    const auto &o = p.options;
    gtk_editable_set_text(GTK_EDITABLE(s->preset_name), p.title.c_str());
    gtk_editable_set_text(GTK_EDITABLE(s->name), o.name.c_str());
    gtk_editable_set_text(GTK_EDITABLE(s->content), o.content.c_str());
    gtk_editable_set_text(GTK_EDITABLE(s->tag), p.tag.c_str());
    gtk_drop_down_set_selected(GTK_DROP_DOWN(s->mode),
                               static_cast<guint>(o.name_mode));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(s->kind), static_cast<guint>(o.kind));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(s->case_sensitive), o.case_sensitive);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(s->recursive), o.recursive);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(s->include_hidden), o.include_hidden);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(s->duplicates), o.duplicates_by_sha256);
    const auto field = [](GtkWidget *target, auto value) {
        const std::string text = value ? std::to_string(*value) : "";
        gtk_editable_set_text(GTK_EDITABLE(target), text.c_str());
    };
    field(s->minimum, o.minimum_size);
    field(s->maximum, o.maximum_size);
    field(s->owner_uid, o.owner_uid);
    set_date_field(s->since, o.modified_since);
    set_date_field(s->before, o.modified_before);
    gtk_label_set_text(GTK_LABEL(s->scope_label),
        ("Saved scope: " + p.root_uri).c_str());
    s->last_options.reset();
    gtk_label_set_text(GTK_LABEL(s->status), "Saved search loaded. Select Search to run.");
}

void remove_preset(GtkButton *, gpointer data)
{
    auto *s = state(GTK_WIDGET(data));
    if (s == nullptr) return;
    auto presets = load_saved_searches();
    if (!presets.writable) {
        gtk_label_set_text(GTK_LABEL(s->status), presets.error.c_str());
        return;
    }
    const guint index = gtk_drop_down_get_selected(GTK_DROP_DOWN(s->preset_choice));
    if (index >= presets.items.size()) return;
    presets.items.erase(presets.items.begin() + static_cast<std::ptrdiff_t>(index));
    std::string error;
    if (!save_saved_searches(presets, error)) {
        gtk_label_set_text(GTK_LABEL(s->status), error.c_str());
        return;
    }
    refresh_presets(s);
    gtk_label_set_text(GTK_LABEL(s->status), "Saved search removed.");
}

void persist_privacy(GtkButton *, gpointer data)
{
    auto *s = state(GTK_WIDGET(data));
    if (s == nullptr) return;
    // Retain the same strict absolute-path/URI parsing as the search operation.
    // The resulting persisted list becomes mandatory on every subsequent scan.
    const char *raw = gtk_editable_get_text(GTK_EDITABLE(s->excluded));
    gchar **parts = g_strsplit(raw != nullptr ? raw : "", ";", 65);
    std::vector<std::string> uris;
    bool valid = true;
    for (gchar **p = parts; p != nullptr && *p != nullptr; ++p) {
        char *part = g_strstrip(*p);
        if (*part == '\0') continue;
        if (!(g_str_has_prefix(part, "/") || strstr(part, "://") != nullptr)) {
            valid = false;
            break;
        }
        GFile *file = strstr(part, "://") != nullptr ?
            g_file_new_for_uri(part) : g_file_new_for_path(part);
        char *uri = g_file_get_uri(file);
        if (uri != nullptr) uris.emplace_back(uri);
        g_free(uri);
        g_object_unref(file);
    }
    g_strfreev(parts);
    if (!valid || uris.size() > 64) {
        gtk_label_set_text(GTK_LABEL(s->status),
            "Private exclusions require absolute locations (maximum 64).");
        return;
    }
    std::string error;
    if (!save_search_privacy(uris, error)) {
        gtk_label_set_text(GTK_LABEL(s->status), error.c_str());
        return;
    }
    gtk_label_set_text(GTK_LABEL(s->status),
        "Private exclusions saved. They apply to every subsequent search.");
}


void show_recursive_search_impl(GtkWindow *owner, GFile *location,
                               const std::vector<GFile *> &selected)
{
    if (owner == nullptr || location == nullptr || selected.size() > 16) return;
    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Search files");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), owner);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_modal(GTK_WINDOW(dialog), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 760, 690);
    auto *s = new SearchWindow;
    s->owner = owner;
    s->root = G_FILE(g_object_ref(location));
    for (GFile *folder : selected)
        s->selected_roots.push_back(G_FILE(g_object_ref(folder)));
    g_object_set_data_full(G_OBJECT(dialog), "ifm-recursive-search-state", s,
        [](gpointer ptr) { delete static_cast<SearchWindow *>(ptr); });

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
    gtk_widget_set_margin_start(outer, 14);
    gtk_widget_set_margin_end(outer, 14);
    gtk_widget_set_margin_top(outer, 14);
    gtk_widget_set_margin_bottom(outer, 14);
    gtk_window_set_child(GTK_WINDOW(dialog), outer);
    char *location_name = g_file_get_parse_name(location);
    const std::string scope = selected.empty() ?
        "Search within: " + std::string(location_name != nullptr ?
                                      location_name : "current folder") :
        "Search within " + std::to_string(selected.size()) +
        " selected folders (at most 16)";
    g_free(location_name);
    GtkWidget *intro = gtk_label_new(scope.c_str());
    s->scope_label = intro;
    gtk_label_set_xalign(GTK_LABEL(intro), 0);
    gtk_label_set_ellipsize(GTK_LABEL(intro), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(outer), intro);

    GtkWidget *saved_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(outer), saved_row);
    s->preset_name = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(s->preset_name), "Saved search name");
    gtk_widget_set_hexpand(s->preset_name, TRUE);
    gtk_box_append(GTK_BOX(saved_row), s->preset_name);
    s->preset_save = gtk_button_new_with_label("Save Search");
    gtk_box_append(GTK_BOX(saved_row), s->preset_save);
    constexpr const char *empty_presets[] = {nullptr};
    s->preset_choice = gtk_drop_down_new_from_strings(empty_presets);
    gtk_box_append(GTK_BOX(saved_row), s->preset_choice);
    GtkWidget *load_saved = gtk_button_new_with_label("Load saved");
    GtkWidget *remove_saved = gtk_button_new_with_label("Remove saved");
    gtk_box_append(GTK_BOX(saved_row), load_saved);
    gtk_box_append(GTK_BOX(saved_row), remove_saved);

    GtkWidget *options = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(options), 14);
    gtk_grid_set_row_spacing(GTK_GRID(options), 8);
    gtk_box_append(GTK_BOX(outer), options);
    // Search criteria precede saved presets in focus/reading order. Besides
    // reducing cognitive noise, this retains the established filename-first
    // entry contract for keyboard users and interaction clients.
    gtk_box_reorder_child_after(GTK_BOX(outer), saved_row, options);
    s->name = field(GTK_GRID(options), "Filename", 0, 0, "Part of name, exact name or *.pdf");
    constexpr const char *modes[] = {"Contains", "Exact", "Wildcard (* and ?)", nullptr};
    s->mode = choice(GTK_GRID(options), "Name matching", 1, 0, modes);
    s->content = field(GTK_GRID(options), "Text within file (1 MiB limit)", 0, 1, "Optional UTF-8 text");
    constexpr const char *kinds[] = {"Files and folders", "Files only", "Folders only", nullptr};
    s->kind = choice(GTK_GRID(options), "Type", 1, 1, kinds);
    s->minimum = field(GTK_GRID(options), "Minimum size in bytes", 0, 2, "Optional");
    s->maximum = field(GTK_GRID(options), "Maximum size in bytes", 1, 2, "Optional");
    s->since = field(GTK_GRID(options), "Modified on/after (local YYYY-MM-DD)", 0, 3, "Optional");
    s->before = field(GTK_GRID(options), "Modified before (local YYYY-MM-DD)", 1, 3, "Optional");
    s->owner_uid = field(GTK_GRID(options), "Owner UID (native providers)", 0, 4, "Optional numeric UID");
    s->tag = field(GTK_GRID(options), "Tag filter (derived catalogue)", 0, 5, "Optional exact label");
    s->excluded = field(GTK_GRID(options), "Exclude locations (paths/URIs separated by ;)", 1, 4, "Optional private locations");
    const auto privacy = load_search_privacy();
    if (privacy.valid) {
        std::string text;
        for (const auto &uri : privacy.excluded_uris) {
            if (!text.empty()) text += "; ";
            text += uri;
        }
        gtk_editable_set_text(GTK_EDITABLE(s->excluded), text.c_str());
    }
    GtkWidget *flags = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    s->recursive = gtk_check_button_new_with_label("Include subfolders");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(s->recursive), TRUE);
    s->case_sensitive = gtk_check_button_new_with_label("Case-sensitive text and names");
    s->include_hidden = gtk_check_button_new_with_label("Include hidden items");
    gtk_box_append(GTK_BOX(flags), s->recursive);
    gtk_box_append(GTK_BOX(flags), s->case_sensitive);
    gtk_box_append(GTK_BOX(flags), s->include_hidden);
    s->duplicates = gtk_check_button_new_with_label(
        "Find duplicate regular files by SHA-256 (8 MiB/file limit)");
    gtk_box_append(GTK_BOX(flags), s->duplicates);
    gtk_grid_attach(GTK_GRID(options), flags, 1, 5, 1, 1);
    s->privacy_save = gtk_button_new_with_label("Save private exclusions");
    gtk_grid_attach(GTK_GRID(options), s->privacy_save, 1, 6, 1, 1);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(outer), toolbar);
    s->search = gtk_button_new_with_label("Search");
    gtk_widget_add_css_class(s->search, "suggested-action");
    s->cancel = gtk_button_new_with_label("Cancel scan");
    gtk_widget_set_sensitive(s->cancel, FALSE);
    gtk_box_append(GTK_BOX(toolbar), s->search);
    gtk_box_append(GTK_BOX(toolbar), s->cancel);
    s->status = gtk_label_new(privacy.valid ?
        "Ready — bounded read-only search" : privacy.error.c_str());
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
    refresh_presets(s);
    if (!privacy.valid) {
        gtk_widget_set_sensitive(s->search, FALSE);
        gtk_widget_set_sensitive(s->privacy_save, FALSE);
    }
    g_signal_connect(s->preset_save, "clicked", G_CALLBACK(save_preset), dialog);
    g_signal_connect(load_saved, "clicked", G_CALLBACK(load_preset), dialog);
    g_signal_connect(remove_saved, "clicked", G_CALLBACK(remove_preset), dialog);
    g_signal_connect(s->privacy_save, "clicked", G_CALLBACK(persist_privacy), dialog);
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

void show_recursive_search(GtkWindow *owner, GFile *location)
{
    show_recursive_search_impl(owner, location, {});
}

void show_selected_location_search(GtkWindow *owner,
                                   const std::vector<GFile *> &directories)
{
    if (directories.empty() || directories.size() > 16) return;
    show_recursive_search_impl(owner, directories.front(), directories);
}

} // namespace infiltrator::files
