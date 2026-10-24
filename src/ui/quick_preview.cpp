// SPDX-License-Identifier: GPL-3.0-or-later
#include "quick_preview.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace infiltrator::files {
namespace {

// Two distinct bounds: no more than 8 MiB is transferred from any provider,
// and a text/hex preview never floods the GTK text buffer.
constexpr goffset kMaximumRead = 8 * 1024 * 1024;
constexpr std::size_t kMaximumText = 256 * 1024;
constexpr std::size_t kMaximumHex = 256;
constexpr std::size_t kMaximumVisibleItems = 4096;

struct Target {
    GFile *file{nullptr};
    std::string name;
    ~Target() { if (file != nullptr) g_object_unref(file); }
    Target(GFile *path, const char *label)
        : file(G_FILE(g_object_ref(path))), name(label != nullptr ? label : "") {}
    Target(Target &&other) noexcept : file(std::exchange(other.file, nullptr)),
                                     name(std::move(other.name)) {}
    Target &operator=(Target &&) = delete;
    Target(const Target &) = delete;
    Target &operator=(const Target &) = delete;
};

enum class Kind { Text, Image, Hex, Information };

struct Outcome {
    Kind kind{Kind::Information};
    std::string text;
    std::string info;
    GdkPixbuf *image{nullptr};
    ~Outcome() { if (image != nullptr) g_object_unref(image); }
};

struct Work {
    GFile *file{nullptr};
    Work(GFile *path) : file(G_FILE(g_object_ref(path))) {}
    ~Work() { g_object_unref(file); }
};

struct Preview {
    GtkWindow *owner{nullptr}; // Owned transient destroys before the owner.
    std::vector<Target> targets;
    std::size_t index{0};
    GtkWidget *heading{nullptr};
    GtkWidget *description{nullptr};
    GtkWidget *text{nullptr};
    GtkWidget *picture{nullptr};
    GtkWidget *stack{nullptr};
    GtkWidget *previous{nullptr};
    GtkWidget *next{nullptr};
    GtkWidget *copy_text{nullptr};
    GtkWidget *zoom_in{nullptr};
    GtkWidget *zoom_out{nullptr};
    GtkWidget *cancel{nullptr};
    GCancellable *active{nullptr};
    guint64 generation{0};
    double zoom{1.0};
    GdkPixbuf *image{nullptr};
    std::string visible_text;
    ~Preview() {
        if (active != nullptr) {
            g_cancellable_cancel(active);
            g_object_unref(active);
        }
        if (image != nullptr) g_object_unref(image);
    }
};

Preview *lookup(GtkWidget *widget)
{
    return static_cast<Preview *>(
        g_object_get_data(G_OBJECT(widget), "ifm-quick-preview"));
}

void status(Preview *state, const std::string &detail)
{
    gtk_label_set_text(GTK_LABEL(state->description), detail.c_str());
}

std::string bytes_label(goffset bytes)
{
    if (bytes < 0) return "Size unavailable";
    char *formatted = g_format_size(static_cast<guint64>(bytes));
    std::string result = formatted != nullptr ? formatted : "Size unavailable";
    g_free(formatted);
    return result;
}

std::string hexadecimal(const std::vector<guint8> &bytes)
{
    std::string output = "Binary preview (first 256 bytes)\n\n";
    for (std::size_t index = 0; index < std::min(bytes.size(), kMaximumHex); index += 16) {
        char line[24] = {};
        std::snprintf(line, sizeof(line), "%08zx  ", index);
        output += line;
        const std::size_t end = std::min(index + 16, bytes.size());
        for (std::size_t offset = index; offset < index + 16; ++offset) {
            if (offset < end) {
                char hex[4] = {};
                std::snprintf(hex, sizeof(hex), "%02x ", bytes[offset]);
                output += hex;
            } else {
                output += "   ";
            }
        }
        output += " |";
        for (std::size_t offset = index; offset < end; ++offset) {
            const guint8 byte = bytes[offset];
            output += byte >= 32 && byte <= 126 ? static_cast<char>(byte) : '.';
        }
        output += "|\n";
    }
    return output;
}

void execute(GTask *task, gpointer, gpointer data, GCancellable *cancel)
{
    auto *job = static_cast<Work *>(data);
    GError *error = nullptr;
    std::unique_ptr<Outcome> result(new Outcome);
    GFileInfo *info = g_file_query_info(job->file,
        "standard::name,standard::type,standard::size,standard::content-type,"
        "standard::is-symlink,time::modified",
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, &error);
    if (info == nullptr) {
        g_task_return_error(task, error);
        return;
    }
    const goffset size = g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE) ?
        g_file_info_get_size(info) : -1;
    const char *mime = g_file_info_get_content_type(info);
    const std::string content_type = mime != nullptr ? mime : "Unknown";
    result->info = bytes_label(size) + " · " + content_type;
    const GFileType kind = g_file_info_get_file_type(info);
    const bool link = g_file_info_get_is_symlink(info);
    g_object_unref(info);
    if (link || kind != G_FILE_TYPE_REGULAR) {
        result->kind = Kind::Information;
        result->text = link ? "Symbolic links are not followed in Quick Look.\n"
                            : "Only regular files can be previewed.\n";
        g_task_return_pointer(task, result.release(),
            [](gpointer pointer) { delete static_cast<Outcome *>(pointer); });
        return;
    }
    if (size < 0 || size > kMaximumRead) {
        result->kind = Kind::Information;
        result->text = "Preview unavailable: the provider does not supply a bounded size, "
                       "or the file exceeds 8 MiB.\nOpen normally to inspect the full file.";
        g_task_return_pointer(task, result.release(),
            [](gpointer pointer) { delete static_cast<Outcome *>(pointer); });
        return;
    }
    GFileInputStream *stream = g_file_read(job->file, cancel, &error);
    if (stream == nullptr) {
        g_task_return_error(task, error);
        return;
    }
    std::vector<guint8> buffer;
    buffer.reserve(static_cast<std::size_t>(size));
    std::array<guint8, 65536> chunk{};
    // Refuse a size change that would turn a previously bounded file into an
    // unlimited remote read. The entire buffer stays worker-owned.
    while (buffer.size() <= static_cast<std::size_t>(kMaximumRead)) {
        const gssize count = g_input_stream_read(
            G_INPUT_STREAM(stream), chunk.data(), chunk.size(), cancel, &error);
        if (count <= 0) break;
        buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + count);
    }
    g_input_stream_close(G_INPUT_STREAM(stream), nullptr, nullptr);
    g_object_unref(stream);
    if (error != nullptr) {
        g_task_return_error(task, error);
        return;
    }
    if (buffer.size() > static_cast<std::size_t>(kMaximumRead)) {
        result->kind = Kind::Information;
        result->text = "Preview unavailable: the file grew beyond the 8 MiB limit.";
    } else if (g_cancellable_set_error_if_cancelled(cancel, &error)) {
        g_task_return_error(task, error);
        return;
    } else if (g_content_type_is_a(content_type.c_str(), "image/*")) {
        // The decoder is confined to the worker, including size negotiation.
        // GTK textures and widgets are exclusively constructed on the UI loop.
        GInputStream *memory = g_memory_input_stream_new_from_data(
            buffer.data(), buffer.size(), nullptr);
        GError *decode_error = nullptr;
        result->image = gdk_pixbuf_new_from_stream_at_scale(
            memory, 1050, 740, TRUE, cancel, &decode_error);
        g_object_unref(memory);
        if (result->image != nullptr) {
            result->kind = Kind::Image;
            result->info += " · " +
                std::to_string(gdk_pixbuf_get_width(result->image)) + " × " +
                std::to_string(gdk_pixbuf_get_height(result->image)) + " preview";
        } else {
            result->kind = Kind::Information;
            result->text = std::string("Image preview unsupported: ") +
                (decode_error != nullptr ? decode_error->message : "No qualified decoder");
        }
        g_clear_error(&decode_error);
    } else {
        const bool text_hint = g_content_type_is_a(content_type.c_str(), "text/plain");
        const bool no_nul = std::find(buffer.begin(), buffer.end(), 0) == buffer.end();
        const bool utf8 = buffer.size() <= kMaximumText &&
            g_utf8_validate(reinterpret_cast<const gchar *>(buffer.data()),
                            static_cast<gssize>(buffer.size()), nullptr);
        if (text_hint && no_nul && utf8) {
            result->kind = Kind::Text;
            result->text.assign(buffer.begin(), buffer.end());
        } else {
            result->kind = Kind::Hex;
            result->text = hexadecimal(buffer);
        }
    }
    if (g_cancellable_set_error_if_cancelled(cancel, &error)) {
        g_task_return_error(task, error);
    } else {
        g_task_return_pointer(task, result.release(),
            [](gpointer pointer) { delete static_cast<Outcome *>(pointer); });
    }
}

void display_image(GtkWidget *dialog)
{
    Preview *state = lookup(dialog);
    if (state == nullptr || state->image == nullptr) return;
    // GTK's contain fit never upscales an image merely because zoom changes.
    const int width = static_cast<int>(650 * state->zoom);
    const int height = static_cast<int>(420 * state->zoom);
    gtk_widget_set_size_request(state->picture, width, height);
    gtk_widget_set_sensitive(state->zoom_in, state->zoom < 2.0);
    gtk_widget_set_sensitive(state->zoom_out, state->zoom > 0.5);
}

void finished(GObject *, GAsyncResult *result, gpointer pointer)
{
    auto *weak = static_cast<GWeakRef *>(pointer);
    GError *error = nullptr;
    std::unique_ptr<Outcome> outcome(static_cast<Outcome *>(
        g_task_propagate_pointer(G_TASK(result), &error)));
    GObject *dialog_object = static_cast<GObject *>(g_weak_ref_get(weak));
    g_weak_ref_clear(weak);
    delete weak;
    if (dialog_object != nullptr) {
        GtkWidget *dialog = GTK_WIDGET(dialog_object);
        Preview *state = lookup(dialog);
        if (state != nullptr) {
            g_clear_object(&state->active);
            gtk_widget_set_sensitive(state->cancel, FALSE);
            if (error != nullptr) {
                gtk_stack_set_visible_child_name(GTK_STACK(state->stack), "text");
                GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->text));
                gtk_text_buffer_set_text(buffer, error->message, -1);
                status(state, "Preview failed");
            } else if (outcome != nullptr) {
                if (state->image != nullptr) g_object_unref(state->image);
                state->image = nullptr;
                if (outcome->kind == Kind::Image && outcome->image != nullptr) {
                    state->image = GDK_PIXBUF(g_object_ref(outcome->image));
                    GdkTexture *texture = gdk_texture_new_for_pixbuf(outcome->image);
                    gtk_picture_set_paintable(GTK_PICTURE(state->picture), GDK_PAINTABLE(texture));
                    g_object_unref(texture);
                    gtk_stack_set_visible_child_name(GTK_STACK(state->stack), "image");
                    state->visible_text.clear();
                } else {
                    gtk_stack_set_visible_child_name(GTK_STACK(state->stack), "text");
                    state->visible_text = outcome->text;
                    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->text));
                    gtk_text_buffer_set_text(buffer, outcome->text.c_str(), -1);
                }
                gtk_widget_set_sensitive(state->copy_text, outcome->kind == Kind::Text ||
                                                    outcome->kind == Kind::Hex);
                const std::string label = outcome->kind == Kind::Text ? "Text preview" :
                    outcome->kind == Kind::Image ? "Image preview" :
                    outcome->kind == Kind::Hex ? "Hex preview" : "Preview unavailable";
                status(state, label + " · " + outcome->info);
                display_image(dialog);
            }
        }
        g_object_unref(dialog_object);
    }
    g_clear_error(&error);
}

void load(GtkWidget *dialog)
{
    Preview *state = lookup(dialog);
    if (state == nullptr || state->targets.empty()) return;
    if (state->active != nullptr) {
        g_cancellable_cancel(state->active);
        g_clear_object(&state->active);
    }
    if (state->image != nullptr) {
        g_object_unref(state->image);
        state->image = nullptr;
    }
    gtk_picture_set_paintable(GTK_PICTURE(state->picture), nullptr);
    state->visible_text.clear();
    gtk_widget_set_sensitive(state->copy_text, FALSE);
    gtk_widget_set_sensitive(state->cancel, TRUE);
    gtk_widget_set_sensitive(state->previous, state->index > 0);
    gtk_widget_set_sensitive(state->next, state->index + 1 < state->targets.size());
    gtk_widget_set_sensitive(state->zoom_in, FALSE);
    gtk_widget_set_sensitive(state->zoom_out, FALSE);
    const Target &target = state->targets[state->index];
    gtk_label_set_text(GTK_LABEL(state->heading), target.name.c_str());
    status(state, "Loading preview…");
    gtk_stack_set_visible_child_name(GTK_STACK(state->stack), "text");
    gtk_text_buffer_set_text(
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->text)), "Loading preview…", -1);
    state->active = g_cancellable_new();
    auto *weak = new GWeakRef;
    g_weak_ref_init(weak, G_OBJECT(dialog));
    GTask *task = g_task_new(nullptr, state->active, finished, weak);
    g_task_set_task_data(task, new Work(target.file),
        [](gpointer data) { delete static_cast<Work *>(data); });
    g_task_run_in_thread(task, execute);
    g_object_unref(task);
}

void navigate(GtkButton *button, gpointer raw)
{
    GtkWidget *dialog = GTK_WIDGET(raw);
    Preview *state = lookup(dialog);
    if (state == nullptr) return;
    if (GTK_WIDGET(button) == state->previous && state->index > 0) --state->index;
    else if (GTK_WIDGET(button) == state->next && state->index + 1 < state->targets.size())
        ++state->index;
    else return;
    load(dialog);
}

GtkWidget *action(GtkWidget *parent, const char *label, GCallback callback,
                  GtkWidget *dialog)
{
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(button, "flat");
    gtk_box_append(GTK_BOX(parent), button);
    if (callback != nullptr)
        g_signal_connect(button, "clicked", callback, dialog);
    return button;
}

void preview(GtkWindow *owner, std::vector<Target> targets, std::size_t selected)
{
    if (targets.empty() || selected >= targets.size()) return;
    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Quick Look");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), owner);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_modal(GTK_WINDOW(dialog), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 800, 570);
    auto *state = new Preview;
    state->owner = owner;
    state->targets = std::move(targets);
    state->index = selected;
    g_object_set_data_full(G_OBJECT(dialog), "ifm-quick-preview", state,
                           [](gpointer data) { delete static_cast<Preview *>(data); });
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(root, 16);
    gtk_widget_set_margin_end(root, 16);
    gtk_widget_set_margin_top(root, 14);
    gtk_widget_set_margin_bottom(root, 14);
    gtk_window_set_child(GTK_WINDOW(dialog), root);
    GtkWidget *heading = gtk_label_new("");
    state->heading = heading;
    gtk_label_set_xalign(GTK_LABEL(heading), 0);
    gtk_widget_add_css_class(heading, "title-2");
    gtk_label_set_ellipsize(GTK_LABEL(heading), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(root), heading);
    state->description = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(state->description), 0);
    gtk_widget_add_css_class(state->description, "dim-label");
    gtk_box_append(GTK_BOX(root), state->description);

    state->stack = gtk_stack_new();
    gtk_widget_set_vexpand(state->stack, TRUE);
    gtk_box_append(GTK_BOX(root), state->stack);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    state->text = gtk_text_view_new();
    gtk_widget_set_can_focus(state->text, FALSE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(state->text), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(state->text), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(state->text), GTK_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(state->text, "monospace");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), state->text);
    gtk_stack_add_named(GTK_STACK(state->stack), scroll, "text");
    GtkWidget *image_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(image_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    state->picture = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(state->picture), GTK_CONTENT_FIT_CONTAIN);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(image_scroll), state->picture);
    gtk_stack_add_named(GTK_STACK(state->stack), image_scroll, "image");

    GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(root), controls);
    state->previous = action(controls, "Previous", G_CALLBACK(navigate), dialog);
    state->next = action(controls, "Next", G_CALLBACK(navigate), dialog);
    state->zoom_out = action(controls, "Zoom −", G_CALLBACK((+[](GtkButton *, gpointer raw) {
        Preview *s = lookup(GTK_WIDGET(raw));
        if (s != nullptr && s->image != nullptr) {
            s->zoom = std::max(0.5, s->zoom - 0.25);
            display_image(GTK_WIDGET(raw));
        }
    })), dialog);
    state->zoom_in = action(controls, "Zoom +", G_CALLBACK((+[](GtkButton *, gpointer raw) {
        Preview *s = lookup(GTK_WIDGET(raw));
        if (s != nullptr && s->image != nullptr) {
            s->zoom = std::min(2.0, s->zoom + 0.25);
            display_image(GTK_WIDGET(raw));
        }
    })), dialog);
    state->copy_text = action(controls, "Copy text", G_CALLBACK((+[](GtkButton *, gpointer raw) {
        Preview *s = lookup(GTK_WIDGET(raw));
        if (s == nullptr || s->visible_text.empty()) return;
        GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(raw));
        if (clipboard != nullptr) gdk_clipboard_set_text(clipboard, s->visible_text.c_str());
    })), dialog);
    action(controls, "Copy path", G_CALLBACK((+[](GtkButton *, gpointer raw) {
        Preview *s = lookup(GTK_WIDGET(raw));
        if (s == nullptr || s->targets.empty()) return;
        GFile *file = s->targets[s->index].file;
        char *value = g_file_get_path(file);
        if (value == nullptr) value = g_file_get_uri(file);
        GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(raw));
        if (clipboard != nullptr && value != nullptr) gdk_clipboard_set_text(clipboard, value);
        g_free(value);
    })), dialog);
    state->cancel = action(controls, "Cancel loading", G_CALLBACK((+[](GtkButton *, gpointer raw) {
        Preview *s = lookup(GTK_WIDGET(raw));
        if (s != nullptr && s->active != nullptr) g_cancellable_cancel(s->active);
    })), dialog);
    GtkWidget *close = action(controls, "Close", G_CALLBACK((+[](GtkButton *, gpointer raw) {
        gtk_window_destroy(GTK_WINDOW(raw));
    })), dialog);
    gtk_widget_set_hexpand(close, TRUE);
    gtk_widget_set_halign(close, GTK_ALIGN_END);
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK((+[](GtkEventControllerKey *, guint key,
        guint, GdkModifierType, gpointer raw) -> gboolean {
        if (key == GDK_KEY_Escape) {
            gtk_window_destroy(GTK_WINDOW(raw));
            return TRUE;
        }
        return FALSE;
    })), dialog);
    gtk_widget_add_controller(dialog, keys);
    gtk_window_present(GTK_WINDOW(dialog));
    load(dialog);
}

} // namespace

void show_quick_preview(GtkWindow *owner, GtkDirectoryList *listing,
                        GtkMultiSelection *visible, GtkSingleSelection *primary)
{
    if (owner == nullptr || listing == nullptr || visible == nullptr || primary == nullptr)
        return;
    GFile *parent = gtk_directory_list_get_file(listing);
    auto *selected = G_FILE_INFO(gtk_single_selection_get_selected_item(primary));
    if (parent == nullptr || selected == nullptr ||
        g_file_info_get_file_type(selected) != G_FILE_TYPE_REGULAR ||
        g_file_info_get_name(selected) == nullptr) return;
    const char *selected_name = g_file_info_get_name(selected);
    GListModel *model = gtk_multi_selection_get_model(visible);
    if (model == nullptr) return;
    const guint count = g_list_model_get_n_items(model);
    std::vector<Target> entries;
    entries.reserve(std::min(static_cast<std::size_t>(count), kMaximumVisibleItems));
    std::size_t selected_index = kMaximumVisibleItems;
    for (guint i = 0; i < count && entries.size() < kMaximumVisibleItems; ++i) {
        auto *info = G_FILE_INFO(g_list_model_get_item(model, i));
        if (info != nullptr && g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR &&
            g_file_info_get_name(info) != nullptr) {
            const char *name = g_file_info_get_name(info);
            GFile *child = g_file_get_child(parent, name);
            if (g_strcmp0(name, selected_name) == 0) selected_index = entries.size();
            entries.emplace_back(child, g_file_info_get_display_name(info));
            g_object_unref(child);
        }
        if (info != nullptr) g_object_unref(info);
    }
    if (selected_index == kMaximumVisibleItems) {
        GFile *child = g_file_get_child(parent, selected_name);
        selected_index = entries.size();
        entries.emplace_back(child, g_file_info_get_display_name(selected));
        g_object_unref(child);
    }
    preview(owner, std::move(entries), selected_index);
}

} // namespace infiltrator::files
