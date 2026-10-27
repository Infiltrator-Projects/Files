// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_inspection.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#ifdef __linux__
#include <sys/xattr.h>
#endif

namespace infiltrator::files {
namespace {

enum class InspectionKind { Details, FolderSize, Sha256, VerifySha256, Compare };

struct Work {
    InspectionKind kind;
    GFile *first;
    GFile *second;
    std::string expected;
    Work(InspectionKind value, GFile *a, GFile *b = nullptr, std::string digest = {})
        : kind(value), first(G_FILE(g_object_ref(a))),
          second(b != nullptr ? G_FILE(g_object_ref(b)) : nullptr),
          expected(std::move(digest)) {}
    ~Work() {
        g_object_unref(first);
        if (second != nullptr) g_object_unref(second);
    }
};

struct DialogState {
    GFile *file{nullptr};
    GtkWidget *view{nullptr};
    GtkWidget *details{nullptr};
    GtkWidget *folder{nullptr};
    GtkWidget *hash{nullptr};
    GtkWidget *expected_hash{nullptr};
    GtkWidget *verify_hash{nullptr};
    GtkWidget *cancel{nullptr};
    GCancellable *active{nullptr};
    bool busy{false};
    ~DialogState() {
        if (active != nullptr) {
            g_cancellable_cancel(active);
            g_object_unref(active);
        }
        if (file != nullptr) g_object_unref(file);
    }
};

std::string display(GFile *file)
{
    char *value = g_file_get_parse_name(file);
    std::string result = value != nullptr ? value : "(unavailable)";
    g_free(value);
    return result;
}

std::string size_text(guint64 bytes)
{
    char *text = g_format_size(bytes);
    std::string result = text != nullptr ? text : "Unknown";
    g_free(text);
    return result;
}

void append_line(std::string &out, const char *label, const std::string &value)
{
    out += label;
    out += ": ";
    out += value;
    out += '\n';
}

void append_time(std::string &out, GFileInfo *info, const char *attribute,
                 const char *label)
{
    if (!g_file_info_has_attribute(info, attribute)) return;
    const guint64 seconds = g_file_info_get_attribute_uint64(info, attribute);
    if (seconds > static_cast<guint64>(G_MAXINT64)) return;
    GDateTime *time = g_date_time_new_from_unix_local(static_cast<gint64>(seconds));
    if (time == nullptr) return;
    char *formatted = g_date_time_format(time, "%Y-%m-%d %H:%M:%S %Z");
    if (formatted != nullptr) append_line(out, label, formatted);
    g_free(formatted);
    g_date_time_unref(time);
}

std::string file_metadata(GFile *file, GCancellable *cancel, GError **error)
{
    constexpr const char *attributes =
        "standard::name,standard::display-name,standard::type,standard::size,"
        "standard::content-type,standard::is-symlink,standard::symlink-target,"
        "time::modified,time::created,time::access,unix::uid,unix::gid,"
        "unix::mode,unix::blocks,owner::user,owner::group";
    GFileInfo *info = g_file_query_info(
        file, attributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, error);
    if (info == nullptr) return {};

    std::string report;
    append_line(report, "Location", display(file));
    char *scheme = g_file_get_uri_scheme(file);
    if (scheme != nullptr) {
        append_line(report, "Location provider", scheme);
        g_free(scheme);
    }
    const char *shown = g_file_info_get_display_name(info);
    if (shown != nullptr) append_line(report, "Name", shown);
    const char *type = g_file_info_get_content_type(info);
    if (type != nullptr) append_line(report, "Content type", type);
    const GFileType kind = g_file_info_get_file_type(info);
    append_line(report, "Kind", kind == G_FILE_TYPE_DIRECTORY ? "Directory" :
                kind == G_FILE_TYPE_REGULAR ? "Regular file" :
                kind == G_FILE_TYPE_SYMBOLIC_LINK ? "Symbolic link" : "Other");
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE) &&
        g_file_info_get_size(info) >= 0)
        append_line(report, "Logical size", size_text(
            static_cast<guint64>(g_file_info_get_size(info))));
    else
        append_line(report, "Logical size", "Unknown");
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_BLOCKS)) {
        const guint64 blocks = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_UNIX_BLOCKS);
        if (blocks <= G_MAXUINT64 / 512U)
            append_line(report, "Allocated size", size_text(blocks * 512U));
    }
    append_time(report, info, G_FILE_ATTRIBUTE_TIME_MODIFIED, "Modified");
    append_time(report, info, G_FILE_ATTRIBUTE_TIME_CREATED, "Created");
    append_time(report, info, G_FILE_ATTRIBUTE_TIME_ACCESS, "Accessed");
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_MODE)) {
        const guint32 mode = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE);
        char *formatted = g_strdup_printf("%04o", mode & 07777U);
        append_line(report, "Permissions", formatted != nullptr ? formatted : "Unknown");
        g_free(formatted);
    }
    const char *owner = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_OWNER_USER);
    const char *group = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_OWNER_GROUP);
    if (owner != nullptr) append_line(report, "Owner", owner);
    if (group != nullptr) append_line(report, "Group", group);
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_UID))
        append_line(report, "Owner ID", std::to_string(
            g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_UID)));
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_GID))
        append_line(report, "Group ID", std::to_string(
            g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_GID)));
    if (g_file_info_get_is_symlink(info)) {
        const char *target = g_file_info_get_symlink_target(info);
        append_line(report, "Link target", target != nullptr ? target : "Unavailable");
        GError *link_error = nullptr;
        GFileInfo *resolved = g_file_query_info(file,
            G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NONE,
            cancel, &link_error);
        if (resolved != nullptr) {
            const GFileType resolved_type = g_file_info_get_file_type(resolved);
            append_line(report, "Link status",
                resolved_type == G_FILE_TYPE_DIRECTORY ? "Target directory available" :
                resolved_type == G_FILE_TYPE_REGULAR ? "Target file available" :
                "Target resolves to another object type");
            g_object_unref(resolved);
        } else if (g_error_matches(link_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) ||
                   g_error_matches(link_error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY)) {
            append_line(report, "Link status", "Broken (target not found)");
        } else {
            append_line(report, "Link status", "Unknown (target could not be checked)");
        }
        g_clear_error(&link_error);
    }
    g_object_unref(info);

    // GIO providers may not expose filesystem metadata. Absence is not zero.
    GError *fs_error = nullptr;
    GFileInfo *fs = g_file_query_filesystem_info(
        file, "filesystem::size,filesystem::free,filesystem::readonly,filesystem::type",
        cancel, &fs_error);
    if (fs != nullptr) {
        const char *filesystem_type =
            g_file_info_get_attribute_string(fs, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE);
        if (filesystem_type != nullptr)
            append_line(report, "Filesystem type", filesystem_type);
        if (g_file_info_has_attribute(fs, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE))
            append_line(report, "Volume capacity", size_text(
                g_file_info_get_attribute_uint64(fs, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE)));
        if (g_file_info_has_attribute(fs, G_FILE_ATTRIBUTE_FILESYSTEM_FREE))
            append_line(report, "Volume free", size_text(
                g_file_info_get_attribute_uint64(fs, G_FILE_ATTRIBUTE_FILESYSTEM_FREE)));
        if (g_file_info_has_attribute(fs, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY))
            append_line(report, "Volume read-only",
                g_file_info_get_attribute_boolean(fs, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY)
                    ? "Yes" : "No");
        g_object_unref(fs);
    }
    g_clear_error(&fs_error);
    if (g_cancellable_set_error_if_cancelled(cancel, error)) return {};
    GError *mount_error = nullptr;
    GMount *mount = g_file_find_enclosing_mount(file, cancel, &mount_error);
    if (mount != nullptr) {
        char *mount_name = g_mount_get_name(mount);
        if (mount_name != nullptr)
            append_line(report, "Mounted volume", mount_name);
        g_free(mount_name);
        g_object_unref(mount);
    }
    g_clear_error(&mount_error);
    if (g_cancellable_set_error_if_cancelled(cancel, error)) return {};

    char *path = g_file_get_path(file);
    if (path != nullptr) {
        int width = 0, height = 0;
        if (gdk_pixbuf_get_file_info(path, &width, &height) != nullptr &&
            width > 0 && height > 0)
            append_line(report, "Image dimensions",
                        std::to_string(width) + " × " + std::to_string(height));
#ifdef __linux__
        // A selected file may expose its received/download origin via ordinary
        // Linux user attributes. Keep it read-only, bounded, and clearly
        // optional: absence is not evidence that a file was not downloaded.
        for (const auto &[key, caption] : {
                 std::pair{"user.xdg.origin.url", "Download origin"},
                 std::pair{"user.xdg.referrer.url", "Download referrer"}}) {
            const ssize_t needed = lgetxattr(path, key, nullptr, 0);
            if (needed <= 0 || needed > 2048) continue;
            std::string value(static_cast<std::size_t>(needed), '\0');
            const ssize_t got = lgetxattr(path, key, value.data(), value.size());
            if (got != needed || !g_utf8_validate(value.c_str(), got, nullptr)) continue;
            bool safe = true;
            for (const unsigned char ch : value)
                if (ch < 32U || ch == 127U) { safe = false; break; }
            if (safe) append_line(report, caption, value);
        }
        // llistxattr never follows the selected final symlink. Bound both the
        // kernel-returned byte count and rendered list; attributes are data,
        // not commands or promises of filesystem integrity.
        errno = 0;
        const ssize_t length = llistxattr(path, nullptr, 0);
        if (length >= 0 && length <= 65536) {
            std::vector<char> names(static_cast<std::size_t>(length));
            const ssize_t read = length > 0 ? llistxattr(path, names.data(), names.size()) : 0;
            if (read >= 0 && read <= length) {
                std::size_t count = 0;
                bool acl = false;
                for (std::size_t i = 0; i < static_cast<std::size_t>(read);) {
                    const std::size_t start = i;
                    while (i < static_cast<std::size_t>(read) && names[i] != '\0') ++i;
                    if (i == static_cast<std::size_t>(read)) break;
                    const std::string name(names.data() + start, i - start);
                    if (name == "system.posix_acl_access") acl = true;
                    if (count < 32) append_line(report, "Extended attribute", name);
                    ++count;
                    ++i;
                }
                append_line(report, "Extended attribute count", std::to_string(count));
                append_line(report, "POSIX access ACL", acl ? "Extended ACL present" :
                            "No extended access ACL listed");
                if (count > 32) report += "Further attribute names omitted.\n";
            }
        } else if (length > 65536) {
            report += "Extended attributes: list exceeds 64 KiB limit.\n";
        }
#endif
        g_free(path);
    }
    return report;
}

std::string checksum(GFile *file, GCancellable *cancel, GError **error)
{
    GFileInputStream *stream = g_file_read(file, cancel, error);
    if (stream == nullptr) return {};
    GChecksum *hash = g_checksum_new(G_CHECKSUM_SHA256);
    std::array<char, 131072> buffer{};
    while (true) {
        const gssize n = g_input_stream_read(
            G_INPUT_STREAM(stream), buffer.data(), static_cast<gsize>(buffer.size()), cancel, error);
        if (n < 0) break;
        if (n == 0) break;
        g_checksum_update(hash, reinterpret_cast<const guchar *>(buffer.data()),
                          static_cast<gsize>(n));
    }
    const std::string value = *error == nullptr ? g_checksum_get_string(hash) : "";
    g_checksum_free(hash);
    g_input_stream_close(G_INPUT_STREAM(stream), nullptr, nullptr);
    g_object_unref(stream);
    return value;
}


bool identical_bytes(GFile *first, GFile *second,
                     GCancellable *cancel, GError **error)
{
    GFileInputStream *left = g_file_read(first, cancel, error);
    if (left == nullptr) return false;
    GFileInputStream *right = g_file_read(second, cancel, error);
    if (right == nullptr) {
        g_input_stream_close(G_INPUT_STREAM(left), nullptr, nullptr);
        g_object_unref(left);
        return false;
    }
    std::array<guint8, 65536> a{}, b{};
    auto chunk = [&](GInputStream *stream, std::array<guint8, 65536> &buffer,
                     gsize &length) {
        length = 0;
        while (length < buffer.size()) {
            const gssize got = g_input_stream_read(stream,
                buffer.data() + length, buffer.size() - length, cancel, error);
            if (got <= 0) return got == 0 && *error == nullptr;
            length += static_cast<gsize>(got);
        }
        return true;
    };
    bool equal = true;
    while (*error == nullptr) {
        gsize l = 0, r = 0;
        if (!chunk(G_INPUT_STREAM(left), a, l) ||
            !chunk(G_INPUT_STREAM(right), b, r)) {
            equal = false;
            break;
        }
        if (l != r || !std::equal(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(l),
                                  b.begin())) {
            equal = false;
            break;
        }
        if (l == 0) break;
    }
    g_input_stream_close(G_INPUT_STREAM(left), nullptr, nullptr);
    g_input_stream_close(G_INPUT_STREAM(right), nullptr, nullptr);
    g_object_unref(left);
    g_object_unref(right);
    return equal && *error == nullptr;
}

std::string folder_size(GFile *root, GCancellable *cancel, GError **error)
{
    struct Frame { GFile *file; GFileEnumerator *enumerator; };
    std::vector<Frame> stack;
    guint64 logical = 0, allocated = 0, items = 0, folders = 0;
    bool logical_known = true, allocated_known = true;
    constexpr guint64 limit = 250000;
    const char *attributes =
        "standard::name,standard::type,standard::size,unix::blocks,unix::device";
    GFileInfo *root_info = g_file_query_info(
        root, attributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, error);
    if (root_info == nullptr) return {};
    const bool has_device = g_file_info_has_attribute(root_info, G_FILE_ATTRIBUTE_UNIX_DEVICE);
    const guint32 device = has_device ?
        g_file_info_get_attribute_uint32(root_info, G_FILE_ATTRIBUTE_UNIX_DEVICE) : 0U;
    g_object_unref(root_info);
    auto push_directory = [&](GFile *folder) {
        GFileEnumerator *e = g_file_enumerate_children(
            folder, attributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, error);
        if (e == nullptr) return false;
        stack.push_back(Frame{G_FILE(g_object_ref(folder)), e});
        return true;
    };
    if (!push_directory(root)) return {};
    while (!stack.empty() && *error == nullptr) {
        if (g_cancellable_set_error_if_cancelled(cancel, error)) break;
        Frame &frame = stack.back();
        GFileInfo *info = g_file_enumerator_next_file(frame.enumerator, cancel, error);
        if (info == nullptr) {
            if (*error != nullptr) break;
            g_object_unref(frame.enumerator);
            g_object_unref(frame.file);
            stack.pop_back();
            continue;
        }
        ++items;
        if (items > limit) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_TOO_MANY_OPEN_FILES,
                        "Folder contains over 250,000 items; calculation is bounded.");
            g_object_unref(info);
            break;
        }
        if (!g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE))
            logical_known = false;
        const goffset raw_size = g_file_info_get_size(info);
        if (raw_size < 0) logical_known = false;
        const guint64 bytes = raw_size >= 0 ? static_cast<guint64>(raw_size) : 0U;
        if (G_MAXUINT64 - logical < bytes) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Logical size overflow");
            g_object_unref(info);
            break;
        }
        logical += bytes;
        if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_BLOCKS)) {
            const guint64 blocks =
                g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_UNIX_BLOCKS);
            if (blocks > G_MAXUINT64 / 512U ||
                G_MAXUINT64 - allocated < blocks * 512U) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Allocated size overflow");
                g_object_unref(info);
                break;
            }
            allocated += blocks * 512U;
        } else {
            allocated_known = false;
        }
        if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
            ++folders;
            const bool crosses_mount = has_device &&
                g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_DEVICE) &&
                g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_DEVICE) != device;
            const char *name = g_file_info_get_name(info);
            if (!crosses_mount && name != nullptr && stack.size() < 64) {
                GFile *child = g_file_get_child(frame.file, name);
                const bool ok = push_directory(child);
                g_object_unref(child);
                if (!ok) {
                    g_object_unref(info);
                    break;
                }
            } else if (!crosses_mount && stack.size() >= 64) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Folder nesting exceeds 64 levels; no partial total reported.");
                g_object_unref(info);
                break;
            }
        }
        g_object_unref(info);
    }
    for (Frame &f : stack) {
        g_object_unref(f.enumerator);
        g_object_unref(f.file);
    }
    if (*error != nullptr) return {};
    std::string result;
    append_line(result, "Folder", display(root));
    append_line(result, "Items scanned", std::to_string(items));
    append_line(result, "Subfolders", std::to_string(folders));
    append_line(result, "Logical total",
                logical_known ? size_text(logical) : "Unknown (some sizes not supplied)");
    append_line(result, "Allocated total",
                allocated_known ? size_text(allocated) : "Unknown (some block counts not supplied)");
    result += "Counts include directory entries; hard links may be counted more than once.\n"
              "Symlinks and separately mounted filesystems are not traversed.\n";
    return result;
}

void work_thread(GTask *task, gpointer, gpointer data, GCancellable *cancel)
{
    auto *work = static_cast<Work *>(data);
    GError *error = nullptr;
    std::string result;
    switch (work->kind) {
    case InspectionKind::Details:
        result = file_metadata(work->first, cancel, &error);
        break;
    case InspectionKind::FolderSize:
        result = folder_size(work->first, cancel, &error);
        break;
    case InspectionKind::Sha256: {
        result = checksum(work->first, cancel, &error);
        if (error == nullptr) result = "SHA-256: " + result + "\n" + display(work->first);
        break;
    }
    case InspectionKind::VerifySha256: {
        const std::string actual = checksum(work->first, cancel, &error);
        if (error != nullptr) break;
        append_line(result, "Source", display(work->first));
        append_line(result, "Expected SHA-256", work->expected);
        append_line(result, "Calculated SHA-256", actual);
        result += actual == work->expected ?
            "SHA-256 VERIFIED: calculated digest matches the supplied value.\n" :
            "SHA-256 MISMATCH: supplied and calculated digests differ.\n";
        result += "A matching digest does not establish trusted provenance.\n";
        break;
    }
    case InspectionKind::Compare: {
        // Source/destination logical sizes belong to the original comparison
        // contract; an unavailable size is never reported as zero.
        for (const auto &item : std::array<std::pair<GFile *, const char *>, 2>{
                 std::pair{work->first, "First logical size"},
                 std::pair{work->second, "Second logical size"}}) {
            GError *size_error = nullptr;
            GFileInfo *info = g_file_query_info(
                item.first, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, &size_error);
            if (info != nullptr &&
                g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE) &&
                g_file_info_get_size(info) >= 0)
                append_line(result, item.second,
                    size_text(static_cast<guint64>(g_file_info_get_size(info))));
            else
                append_line(result, item.second, "Unavailable");
            if (info != nullptr) g_object_unref(info);
            g_clear_error(&size_error);
        }
        if (g_cancellable_set_error_if_cancelled(cancel, &error)) break;
        const std::string first = checksum(work->first, cancel, &error);
        if (error != nullptr) break;
        const std::string second = checksum(work->second, cancel, &error);
        if (error != nullptr) break;
        append_line(result, "First", display(work->first));
        append_line(result, "Second", display(work->second));
        append_line(result, "First SHA-256", first);
        append_line(result, "Second SHA-256", second);
        result += first == second ? "Matching SHA-256 digests.\n" :
                                   "Different SHA-256 digests.\n";
        if (first == second) {
            const bool identical = identical_bytes(work->first, work->second, cancel, &error);
            if (error != nullptr) break;
            result += identical ? "Byte-for-byte comparison matched on this read.\n" :
                                  "Byte-for-byte comparison found differences.\n";
            result += "Concurrent file modifications can invalidate a live comparison.\n";
        }
        break;
    }
    }
    if (error != nullptr) {
        g_task_return_error(task, error);
    } else {
        g_task_return_pointer(task, g_strdup(result.c_str()), g_free);
    }
}

void set_text(GtkWidget *view, const char *value)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_set_text(buffer, value, -1);
}

void on_done(GObject *, GAsyncResult *result, gpointer user_data)
{
    auto *weak = static_cast<GWeakRef *>(user_data);
    GError *error = nullptr;
    char *text = static_cast<char *>(g_task_propagate_pointer(G_TASK(result), &error));
    GObject *object = static_cast<GObject *>(g_weak_ref_get(weak));
    if (object != nullptr) {
        auto *state = static_cast<DialogState *>(
            g_object_get_data(object, "ifm-inspection-state"));
        if (state != nullptr) {
            state->busy = false;
            if (state->active != nullptr) {
                g_object_unref(state->active);
                state->active = nullptr;
            }
            if (state->details != nullptr) gtk_widget_set_sensitive(state->details, TRUE);
            if (state->folder != nullptr) gtk_widget_set_sensitive(state->folder, TRUE);
            if (state->hash != nullptr) gtk_widget_set_sensitive(state->hash, TRUE);
            if (state->verify_hash != nullptr) gtk_widget_set_sensitive(state->verify_hash, TRUE);
            if (state->cancel != nullptr) gtk_widget_set_sensitive(state->cancel, FALSE);
            if (error != nullptr)
                set_text(state->view, error->message);
            else if (text != nullptr)
                set_text(state->view, text);
        }
        g_object_unref(object);
    }
    g_free(text);
    g_clear_error(&error);
    g_weak_ref_clear(weak);
    delete weak;
}

void begin(GtkWindow *dialog, InspectionKind kind, GFile *other = nullptr,
           std::string expected = {})
{
    auto *state = static_cast<DialogState *>(
        g_object_get_data(G_OBJECT(dialog), "ifm-inspection-state"));
    if (state == nullptr || state->busy) return;
    state->busy = true;
    state->active = g_cancellable_new();
    if (state->details != nullptr) gtk_widget_set_sensitive(state->details, FALSE);
    if (state->folder != nullptr) gtk_widget_set_sensitive(state->folder, FALSE);
    if (state->hash != nullptr) gtk_widget_set_sensitive(state->hash, FALSE);
    if (state->verify_hash != nullptr) gtk_widget_set_sensitive(state->verify_hash, FALSE);
    if (state->cancel != nullptr) gtk_widget_set_sensitive(state->cancel, TRUE);
    set_text(state->view, "Reading… (Cancel to stop)");
    auto *weak = new GWeakRef;
    g_weak_ref_init(weak, G_OBJECT(dialog));
    GTask *task = g_task_new(nullptr, state->active, on_done, weak);
    g_task_set_task_data(task, new Work(kind, state->file, other, std::move(expected)),
        [](gpointer data) { delete static_cast<Work *>(data); });
    g_task_run_in_thread(task, work_thread);
    g_object_unref(task);
}

GtkWindow *make_dialog(GtkWindow *parent, GFile *file, const char *title)
{
    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), title);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), parent);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 600, 480);

    auto *state = new DialogState;
    state->file = G_FILE(g_object_ref(file));
    g_object_set_data_full(G_OBJECT(dialog), "ifm-inspection-state", state,
                           [](gpointer data) { delete static_cast<DialogState *>(data); });
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(root, 14);
    gtk_widget_set_margin_end(root, 14);
    gtk_widget_set_margin_top(root, 14);
    gtk_widget_set_margin_bottom(root, 14);
    gtk_window_set_child(GTK_WINDOW(dialog), root);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    state->view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(state->view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(state->view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(state->view), GTK_WRAP_WORD_CHAR);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), state->view);
    gtk_box_append(GTK_BOX(root), scroll);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(root), buttons);
    GtkWidget *copy = gtk_button_new_with_label("Copy details");
    gtk_box_append(GTK_BOX(buttons), copy);
    g_signal_connect(copy, "clicked", G_CALLBACK((+[](GtkButton *, gpointer data) {
        auto *state = static_cast<DialogState *>(
            g_object_get_data(G_OBJECT(data), "ifm-inspection-state"));
        if (state == nullptr) return;
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->view));
        GtkTextIter begin_iter, end_iter;
        gtk_text_buffer_get_bounds(buffer, &begin_iter, &end_iter);
        char *text = gtk_text_buffer_get_text(buffer, &begin_iter, &end_iter, FALSE);
        GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(data));
        if (clipboard != nullptr && text != nullptr)
            gdk_clipboard_set_text(clipboard, text);
        g_free(text);
    })), dialog);
    state->cancel = gtk_button_new_with_label("Cancel");
    gtk_box_append(GTK_BOX(buttons), state->cancel);
    gtk_widget_set_sensitive(state->cancel, FALSE);
    g_signal_connect(state->cancel, "clicked", G_CALLBACK((+[](GtkButton *, gpointer data) {
        auto *state = static_cast<DialogState *>(
            g_object_get_data(G_OBJECT(data), "ifm-inspection-state"));
        if (state != nullptr && state->active != nullptr)
            g_cancellable_cancel(state->active);
    })), dialog);
    GtkWidget *close = gtk_button_new_with_label("Close");
    gtk_widget_set_hexpand(close, TRUE);
    gtk_widget_set_halign(close, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(buttons), close);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_destroy), dialog);
    return GTK_WINDOW(dialog);
}

} // namespace

void show_file_inspection(GtkWindow *parent, GFile *file, GFileInfo *known)
{
    if (parent == nullptr || file == nullptr || known == nullptr) return;
    GtkWindow *dialog = make_dialog(parent, file, "Inspect file");
    auto *state = static_cast<DialogState *>(
        g_object_get_data(G_OBJECT(dialog), "ifm-inspection-state"));
    GtkWidget *buttons = gtk_widget_get_last_child(gtk_window_get_child(dialog));
    state->details = gtk_button_new_with_label("Details");
    gtk_box_prepend(GTK_BOX(buttons), state->details);
    g_signal_connect(state->details, "clicked", G_CALLBACK((+[](GtkButton *, gpointer data) {
        begin(GTK_WINDOW(data), InspectionKind::Details);
    })), dialog);
    if (g_file_info_get_file_type(known) == G_FILE_TYPE_DIRECTORY) {
        state->folder = gtk_button_new_with_label("Calculate folder size");
        gtk_box_prepend(GTK_BOX(buttons), state->folder);
        g_signal_connect(state->folder, "clicked", G_CALLBACK((+[](GtkButton *, gpointer data) {
            begin(GTK_WINDOW(data), InspectionKind::FolderSize);
        })), dialog);
    } else if (g_file_info_get_file_type(known) == G_FILE_TYPE_REGULAR) {
        state->hash = gtk_button_new_with_label("SHA-256");
        gtk_box_prepend(GTK_BOX(buttons), state->hash);
        g_signal_connect(state->hash, "clicked", G_CALLBACK((+[](GtkButton *, gpointer data) {
            begin(GTK_WINDOW(data), InspectionKind::Sha256);
        })), dialog);
        GtkWidget *root = gtk_window_get_child(dialog);
        GtkWidget *readout = gtk_widget_get_first_child(root);
        GtkWidget *verify_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_insert_child_after(GTK_BOX(root), verify_row, readout);
        state->expected_hash = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(state->expected_hash),
            "SHA-256 or sha256sum line for this filename");
        gtk_widget_set_hexpand(state->expected_hash, TRUE);
        gtk_box_append(GTK_BOX(verify_row), state->expected_hash);
        state->verify_hash = gtk_button_new_with_label("Verify SHA-256");
        gtk_box_append(GTK_BOX(verify_row), state->verify_hash);
        g_signal_connect(state->verify_hash, "clicked", G_CALLBACK((+[](GtkButton *, gpointer data) {
            GtkWindow *dialog = GTK_WINDOW(data);
            auto *state = static_cast<DialogState *>(
                g_object_get_data(G_OBJECT(dialog), "ifm-inspection-state"));
            if (state == nullptr || state->busy) return;
            const char *input = gtk_editable_get_text(GTK_EDITABLE(state->expected_hash));
            const std::string supplied = input != nullptr ? input : "";
            if (supplied.size() > 8192) {
                set_text(state->view, "Checksum input exceeds its 8 KiB safety limit.");
                return;
            }
            std::string expected = supplied;
            if (supplied.size() > 64) {
                // Standard sha256sum syntax: 64 hex digits, a separator,
                // then ' ' or '*' and the exact selected basename. A manifest
                // entry for a different file must never verify this object.
                char *basename = g_file_get_basename(state->file);
                const std::string target = basename != nullptr ? basename : "";
                g_free(basename);
                if (supplied.size() < 67 || supplied[64] != ' ' ||
                    (supplied[65] != ' ' && supplied[65] != '*') ||
                    supplied.substr(66) != target) {
                    set_text(state->view,
                        "Use 64 SHA-256 hex digits or one sha256sum line for this exact filename.");
                    return;
                }
                expected = supplied.substr(0, 64);
            }
            if (expected.size() != 64 ||
                !std::all_of(expected.begin(), expected.end(), [](unsigned char ch) {
                    return g_ascii_isxdigit(ch);
                })) {
                set_text(state->view, "Enter exactly 64 hexadecimal SHA-256 digits.");
                return;
            }
            for (char &ch : expected)
                ch = g_ascii_tolower(ch);
            begin(dialog, InspectionKind::VerifySha256, nullptr, std::move(expected));
        })), dialog);
    }
    gtk_window_present(dialog);
    begin(dialog, InspectionKind::Details);
}

void compare_file_hashes(GtkWindow *parent, GFile *first, GFile *second)
{
    if (parent == nullptr || first == nullptr || second == nullptr) return;
    GtkWindow *dialog = make_dialog(parent, first, "Compare file hashes");
    gtk_window_present(dialog);
    begin(dialog, InspectionKind::Compare, second);
}

void open_terminal_in_directory(GtkWindow *parent, GFile *directory)
{
    if (parent == nullptr || directory == nullptr) return;
    char *path = g_file_get_path(directory);
    if (path == nullptr) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Terminal requires a native folder");
        gtk_alert_dialog_show(alert, parent);
        g_object_unref(alert);
        return;
    }
    constexpr const char *programs[] = {
        "x-terminal-emulator", "gnome-terminal", "mate-terminal",
        "xfce4-terminal", "konsole", "kgx", "xterm"
    };
    GError *error = nullptr;
    bool launched = false;
    for (const char *program : programs) {
        char *binary = g_find_program_in_path(program);
        if (binary == nullptr) continue;
        GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
        g_subprocess_launcher_set_cwd(launcher, path);
        const char *argv[] = {binary, nullptr};
        GSubprocess *process = g_subprocess_launcher_spawnv(launcher, argv, &error);
        g_object_unref(launcher);
        g_free(binary);
        if (process != nullptr) {
            g_object_unref(process);
            launched = true;
            break;
        }
        g_clear_error(&error);
    }
    g_free(path);
    if (!launched) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("No supported terminal could be started");
        gtk_alert_dialog_show(alert, parent);
        g_object_unref(alert);
    }
}

} // namespace infiltrator::files
