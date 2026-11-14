// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Files chooses a volume and formatter. The privileged host supplies
 * exclusive partition I/O to the independently installed filesystem module.
 */
#include "volume_format_controller.hpp"
#include "../core/physical_media.h"
#include <gio/gio.h>
#include <infiltratr/filesystem_module_v2.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace infiltrator::files {
namespace {
constexpr const char *kWorker = "/System/Commands/files-volume-format";
struct Target {
    GMount *mount{nullptr};
    GVolume *volume{nullptr};
    std::string device;
    std::string mount_name;
    unsigned major_number{0};
    unsigned minor_number{0};
    ~Target() {
        if (mount != nullptr) g_object_unref(mount);
        if (volume != nullptr) g_object_unref(volume);
    }
    Target() = default;
    Target(const Target &) = delete;
    Target &operator=(const Target &) = delete;
};
struct Format {
    std::string id;
    std::string module;
};
struct Request {
    Target target;
    std::vector<Format> formats;
    std::string current_format;
    std::string catalogue_error;
    unsigned queries_pending{0};
    bool ready{false};
    GtkWidget *current{nullptr};
    GtkDropDown *format{nullptr};
    GtkWidget *variant{nullptr};
    GtkWidget *label{nullptr};
    GtkWidget *entry{nullptr};
    GtkWidget *status{nullptr};
    GtkWidget *confirm{nullptr};
    Format selected;
    std::string selected_variant;
    std::string selected_label;
    std::string pending_error;
};
bool find_target(GFile *folder, Target &out)
{
    if (folder == nullptr || !g_file_is_native(folder) ||
        !g_file_test(kWorker, G_FILE_TEST_IS_EXECUTABLE))
        return false;
    GError *error = nullptr;
    GMount *mount = g_file_find_enclosing_mount(folder, nullptr, &error);
    g_clear_error(&error);
    if (mount == nullptr) return false;
    bool valid = false;
    if (!g_mount_is_shadowed(mount) && g_mount_can_unmount(mount)) {
        GFile *root = g_mount_get_root(mount);
        GFile *system_root = g_file_new_for_path("/");
        const bool is_root = root == nullptr || g_file_equal(root, system_root);
        const bool selected_volume_root = root != nullptr && g_file_equal(folder, root);
        g_object_unref(system_root);
        if (root != nullptr) g_object_unref(root);
        if (!is_root && selected_volume_root) {
            GVolume *volume = g_mount_get_volume(mount);
            if (volume != nullptr) {
                char *device = g_volume_get_identifier(
                    volume, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
                struct stat st{};
                if (device != nullptr &&
                    std::strncmp(device, "/dev/", 5) == 0 &&
                    std::strchr(device + 5, '/') == nullptr &&
                    lstat(device, &st) == 0 && S_ISBLK(st.st_mode) &&
                    ifm_partition_is_known(
                        static_cast<unsigned>(major(st.st_rdev)),
                        static_cast<unsigned>(minor(st.st_rdev)))) {
                    out.device = device;
                    // Retain the volume across unmount so an abort before
                    // formatting starts can safely restore its mount.
                    out.volume = G_VOLUME(g_object_ref(volume));
                    out.major_number = static_cast<unsigned>(major(st.st_rdev));
                    out.minor_number = static_cast<unsigned>(minor(st.st_rdev));
                    out.mount_name = "the selected volume";
                    if (char *name = g_mount_get_name(mount); name != nullptr) {
                        out.mount_name = name;
                        g_free(name);
                    }
                    valid = true;
                }
                g_free(device);
                g_object_unref(volume);
            }
        }
    }
    if (valid) out.mount = mount;
    else g_object_unref(mount);
    return valid;
}
void set_status(Request *req, const char *message)
{
    if (req != nullptr && GTK_IS_LABEL(req->status))
        gtk_label_set_text(GTK_LABEL(req->status), message);
}
Request *request(GtkWindow *dialog)
{
    return static_cast<Request *>(g_object_get_data(G_OBJECT(dialog), "ifm-format-request"));
}
void complete(GtkWindow *dialog, const char *message)
{
    set_status(request(dialog), message);
    gtk_window_set_deletable(dialog, TRUE);
}

std::string format_name(const std::string &id)
{
    if (id == "exfat") return "exFAT";
    char *upper = g_ascii_strup(id.c_str(), -1);
    std::string result = upper;
    g_free(upper);
    return result;
}

// Common V2 accepts an exact provider-defined variant. These are UI defaults
// for existing modules. Every discovered formatter accepts an explicit value.
const char *default_variant(std::string_view id)
{
    constexpr std::pair<std::string_view, const char *> defaults[] = {
        {"fat12", "12"}, {"fat16", "16"}, {"fat32", "32"},
        {"exfat", "64"}, {"ntfs", "1"}, {"ofs", "0"}, {"swap", "4096"}
    };
    for (const auto &[format, value] : defaults)
        if (format == id) return value;
    return "";
}

const Format *selected_format(const Request *req)
{
    const guint index = gtk_drop_down_get_selected(req->format);
    return index < req->formats.size() ? &req->formats[index] : nullptr;
}

bool variant_valid(std::string_view text)
{
    if (text.empty()) return false;
    std::uint32_t value{};
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

void update_confirmation(Request *req)
{
    gtk_widget_set_sensitive(req->confirm, req->ready && selected_format(req) &&
        variant_valid(gtk_editable_get_text(GTK_EDITABLE(req->variant))) &&
        std::strcmp(gtk_editable_get_text(GTK_EDITABLE(req->entry)), "FORMAT") == 0);
}

void on_format_selected(GObject *, GParamSpec *, gpointer data)
{
    auto *req = static_cast<Request *>(data);
    const Format *format = selected_format(req);
    gtk_editable_set_text(GTK_EDITABLE(req->variant),
                          format ? default_variant(format->id) : "");
    gtk_editable_set_text(GTK_EDITABLE(req->entry), "");
    gtk_widget_set_sensitive(req->variant, req->ready && format);
    gtk_widget_set_sensitive(req->label, req->ready && format);
    gtk_widget_set_sensitive(req->entry, req->ready && format);
    update_confirmation(req);
}

void finish_discovery(Request *req)
{
    std::sort(req->formats.begin(), req->formats.end(), [](const Format &a, const Format &b) {
        return std::tie(a.id, a.module) < std::tie(b.id, b.module);
    });
    std::string current = "Current filesystem could not be detected.";
    if (!req->current_format.empty())
        current = "Current filesystem: " + format_name(req->current_format);
    gtk_label_set_text(GTK_LABEL(req->current), current.c_str());
    if (req->formats.empty()) {
        set_status(req, req->catalogue_error.empty() ?
            "No installed filesystem module offers Common v2 formatting." :
            req->catalogue_error.c_str());
        return;
    }
    GtkStringList *names = gtk_string_list_new(nullptr);
    guint match = GTK_INVALID_LIST_POSITION;
    unsigned matching = 0;
    for (guint i = 0; i < req->formats.size(); ++i) {
        const auto &format = req->formats[i];
        std::string name = format_name(format.id) + " (" + format.module + ")";
        gtk_string_list_append(names, name.c_str());
        if (format.id == req->current_format) { match = i; ++matching; }
    }
    gtk_drop_down_set_model(req->format, G_LIST_MODEL(names));
    g_object_unref(names);
    req->ready = true;
    gtk_widget_set_sensitive(GTK_WIDGET(req->format), TRUE);
    gtk_drop_down_set_selected(req->format, matching == 1 ? match : GTK_INVALID_LIST_POSITION);
    on_format_selected(nullptr, nullptr, req);
    if (!req->current_format.empty() && matching == 0) {
        const std::string message = "No installed formatter for " +
            format_name(req->current_format) + ". Choose a different filesystem to convert the volume.";
        set_status(req, message.c_str());
    } else {
        set_status(req, "Choose the filesystem to create on this volume.");
    }
}

struct Discovery {
    GtkWindow *dialog;
    bool catalogue;
};

void on_discovered(GObject *object, GAsyncResult *result, gpointer data)
{
    const std::unique_ptr<Discovery> query(static_cast<Discovery *>(data));
    Request *req = request(query->dialog);
    GError *error = nullptr;
    char *out = nullptr;
    GSubprocess *process = G_SUBPROCESS(object);
    if (g_subprocess_communicate_utf8_finish(process, result, &out, nullptr, &error) &&
        g_subprocess_get_successful(process) && out != nullptr) {
        std::istringstream input(out);
        if (query->catalogue) {
            for (std::string line; std::getline(input, line);) {
                const auto separator = line.find('\t');
                if (separator == std::string::npos) continue;
                Format format{line.substr(0, separator), line.substr(separator + 1)};
                if (ifs_fs_format_id_valid(format.id.c_str()) && !format.module.empty() &&
                    format.module.find_first_of(" \t/\r") == std::string::npos)
                    req->formats.push_back(std::move(format));
            }
        } else {
            // lsblk gets the on-disk type from device metadata. Mount driver
            // names such as ntfs3 or fuseblk are not filesystem identities.
            std::string version;
            input >> req->current_format >> version;
            if (req->current_format == "vfat" &&
                (version == "FAT12" || version == "FAT16" || version == "FAT32")) {
                char *lower = g_ascii_strdown(version.c_str(), -1);
                req->current_format = lower;
                g_free(lower);
            }
        }
    } else if (query->catalogue) {
        req->catalogue_error = "Could not discover installed filesystem formatters.";
    }
    g_free(out);
    g_clear_error(&error);
    if (--req->queries_pending == 0) finish_discovery(req);
    g_object_unref(query->dialog);
}

void discover(GtkWindow *dialog, const char *const *argv, bool catalogue)
{
    Request *req = request(dialog);
    GSubprocess *process = g_subprocess_newv(argv,
        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                     G_SUBPROCESS_FLAGS_STDERR_SILENCE), nullptr);
    if (!process) {
        if (catalogue) req->catalogue_error = "Could not start filesystem formatter discovery.";
        return;
    }
    ++req->queries_pending;
    g_subprocess_communicate_utf8_async(process, nullptr, nullptr, on_discovered,
                                       new Discovery{GTK_WINDOW(g_object_ref(dialog)), catalogue});
    g_object_unref(process);
}
/* Remount only when the formatter definitely did not execute. A failed
 * formatter may have modified on-disk metadata and must remain unmounted. */
void on_safe_remount(GObject *object, GAsyncResult *result, gpointer data)
{
    GtkWindow *dialog = GTK_WINDOW(data);
    Request *req = request(dialog);
    GError *error = nullptr;
    const bool restored = g_volume_mount_finish(G_VOLUME(object), result, &error) ||
        (error != nullptr &&
         g_error_matches(error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED));
    std::string message = req != nullptr ? req->pending_error :
        "The formatter did not start.";
    if (restored) {
        message += "\nThe original volume was remounted.";
    } else {
        message += "\nThe volume remains unmounted: ";
        message += error != nullptr ? error->message : "remount failed";
    }
    complete(dialog, message.c_str());
    g_clear_error(&error);
    g_object_unref(dialog);
}

void restore_after_preformat_failure(GtkWindow *dialog,
                                     const std::string& reason)
{
    Request *req = request(dialog);
    if (req == nullptr || req->target.volume == nullptr) {
        const std::string message = reason +
            "\nThe volume remains unmounted; the formatter did not start.";
        complete(dialog, message.c_str());
        return;
    }
    req->pending_error = reason;
    set_status(req, "The formatter did not start. Restoring the original mount.");
    g_volume_mount(req->target.volume, G_MOUNT_MOUNT_NONE, nullptr, nullptr,
                   on_safe_remount, g_object_ref(dialog));
}

void on_process_done(GObject *object, GAsyncResult *result, gpointer data)
{
    GtkWindow *dialog = GTK_WINDOW(data);
    GError *error = nullptr;
    char *out = nullptr;
    char *err = nullptr;
    const bool completed = g_subprocess_communicate_utf8_finish(
        G_SUBPROCESS(object), result, &out, &err, &error);
    const bool success = completed &&
        g_subprocess_get_successful(G_SUBPROCESS(object));
    if (success) {
        const std::string message = "Volume formatted and verified as " +
            format_name(request(dialog)->selected.id) + ".";
        complete(dialog, message.c_str());
    } else {
        const char *reason = error != nullptr ? error->message :
            err != nullptr && *err ? err : "Volume format host failed.";
        const std::string message = std::string("Formatting failed: ") + reason;
        GSubprocess *process = G_SUBPROCESS(object);
        // The helper reserves 2 for failures before writing. pkexec
        // uses 126 and 127 for authorisation or execution refusals.
        const bool never_started = completed &&
            g_subprocess_get_if_exited(process) &&
            (g_subprocess_get_exit_status(process) == 2 ||
             g_subprocess_get_exit_status(process) == 126 ||
             g_subprocess_get_exit_status(process) == 127);
        if (never_started) {
            restore_after_preformat_failure(dialog, message);
        } else {
            const std::string warning = message +
                "\nThe volume remains unmounted because formatting may have "
                "modified filesystem metadata.";
            complete(dialog, warning.c_str());
        }
    }
    g_free(out);
    g_free(err);
    g_clear_error(&error);
    g_object_unref(dialog);
}
void on_unmounted(GObject *object, GAsyncResult *result, gpointer data)
{
    GtkWindow *dialog = GTK_WINDOW(data);
    Request *req = request(dialog);
    GError *error = nullptr;
    if (!g_mount_unmount_with_operation_finish(G_MOUNT(object), result, &error)) {
        std::string message = std::string("Could not unmount volume: ") +
            (error != nullptr ? error->message : "Unknown error");
        req->ready = true;
        gtk_widget_set_sensitive(GTK_WIDGET(req->format), TRUE);
        gtk_widget_set_sensitive(req->variant, TRUE);
        gtk_widget_set_sensitive(req->label, TRUE);
        gtk_widget_set_sensitive(req->entry, TRUE);
        update_confirmation(req);
        complete(dialog, message.c_str());
        g_clear_error(&error);
        g_object_unref(dialog);
        return;
    }
    const std::string major_number = std::to_string(req->target.major_number);
    const std::string minor_number = std::to_string(req->target.minor_number);
    const char *const argv[] = {
        "pkexec", kWorker, "--format", req->selected.id.c_str(),
        req->selected.module.c_str(), req->selected_variant.c_str(), req->target.device.c_str(),
        major_number.c_str(), minor_number.c_str(), req->selected_label.c_str(), nullptr
    };
    GSubprocess *process = g_subprocess_newv(
        argv, static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                           G_SUBPROCESS_FLAGS_STDERR_PIPE),
        &error);
    if (process == nullptr) {
        std::string message = std::string("Could not start privileged host: ") +
            (error != nullptr ? error->message : "Unknown error");
        restore_after_preformat_failure(dialog, message);
        g_clear_error(&error);
        g_object_unref(dialog);
        return;
    }
    set_status(req, "Volume unmounted. Waiting for authentication and formatting.");
    g_subprocess_communicate_utf8_async(
        process, nullptr, nullptr, on_process_done, g_object_ref(dialog));
    g_object_unref(process);
    g_object_unref(dialog);
}
void on_confirm(GtkButton *, gpointer data)
{
    GtkWindow *dialog = GTK_WINDOW(data);
    Request *req = request(dialog);
    if (!req || !gtk_widget_get_sensitive(req->confirm)) return;
    const Format *format = selected_format(req);
    if (!format) return;
    req->selected = *format;
    req->selected_variant = gtk_editable_get_text(GTK_EDITABLE(req->variant));
    req->selected_label = gtk_editable_get_text(GTK_EDITABLE(req->label));
    req->ready = false;
    gtk_widget_set_sensitive(GTK_WIDGET(req->format), FALSE);
    gtk_widget_set_sensitive(req->variant, FALSE);
    gtk_widget_set_sensitive(req->label, FALSE);
    gtk_widget_set_sensitive(req->entry, FALSE);
    gtk_widget_set_sensitive(req->confirm, FALSE);
    gtk_window_set_deletable(dialog, FALSE);
    set_status(req, "Unmounting the selected volume before any formatting.");
    GtkWindow *parent = gtk_window_get_transient_for(dialog);
    GMountOperation *operation = gtk_mount_operation_new(parent);
    g_mount_unmount_with_operation(
        req->target.mount, G_MOUNT_UNMOUNT_NONE,
        operation, nullptr, on_unmounted, g_object_ref(dialog));
    g_object_unref(operation);
}
void on_entry_changed(GtkEditable *, gpointer data)
{
    update_confirmation(static_cast<Request *>(data));
}
} // namespace

bool volume_format_available(GFile *folder)
{
    Target target;
    return find_target(folder, target);
}
void show_volume_format_dialog(GtkWindow *parent, GFile *folder)
{
    if (!GTK_IS_WINDOW(parent)) return;
    auto *req = new Request;
    if (!find_target(folder, req->target)) {
        delete req;
        return;
    }
    GtkWindow *dialog = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(dialog, "Format containing volume");
    gtk_window_set_transient_for(dialog, parent);
    gtk_window_set_modal(dialog, TRUE);
    gtk_window_set_default_size(dialog, 560, -1);
    GtkWidget *layout = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(layout, 16);
    gtk_widget_set_margin_end(layout, 16);
    gtk_widget_set_margin_top(layout, 16);
    gtk_widget_set_margin_bottom(layout, 16);
    std::string message =
        "Formatting erases all files on this volume.\n\n" +
        req->target.mount_name + "\n" + req->target.device;
    GtkWidget *warning = gtk_label_new(message.c_str());
    gtk_label_set_wrap(GTK_LABEL(warning), TRUE);
    gtk_label_set_xalign(GTK_LABEL(warning), 0.0F);
    gtk_box_append(GTK_BOX(layout), warning);

    req->current = gtk_label_new("Detecting the current filesystem…");
    gtk_label_set_xalign(GTK_LABEL(req->current), 0.0F);
    gtk_box_append(GTK_BOX(layout), req->current);

    req->format = GTK_DROP_DOWN(gtk_drop_down_new(nullptr, nullptr));
    gtk_accessible_update_property(GTK_ACCESSIBLE(req->format),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, "New filesystem", -1);
    gtk_widget_set_sensitive(GTK_WIDGET(req->format), FALSE);
    gtk_box_append(GTK_BOX(layout), GTK_WIDGET(req->format));

    req->variant = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(req->variant), "Provider variant number");
    gtk_widget_set_tooltip_text(req->variant, "Numeric format variant defined by the selected module");
    gtk_entry_set_input_purpose(GTK_ENTRY(req->variant), GTK_INPUT_PURPOSE_DIGITS);
    gtk_widget_set_sensitive(req->variant, FALSE);
    gtk_box_append(GTK_BOX(layout), req->variant);

    req->label = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(req->label), "Volume label (if supported)");
    gtk_widget_set_sensitive(req->label, FALSE);
    gtk_box_append(GTK_BOX(layout), req->label);

    req->entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(req->entry), "Type FORMAT to confirm");
    gtk_box_append(GTK_BOX(layout), req->entry);
    gtk_widget_set_sensitive(req->entry, FALSE);
    req->confirm = gtk_button_new_with_label("Erase and format volume");
    gtk_widget_set_sensitive(req->confirm, FALSE);
    gtk_box_append(GTK_BOX(layout), req->confirm);
    req->status = gtk_label_new("Looking for installed filesystem formatters…");
    gtk_label_set_wrap(GTK_LABEL(req->status), TRUE);
    gtk_label_set_xalign(GTK_LABEL(req->status), 0.0F);
    gtk_box_append(GTK_BOX(layout), req->status);
    gtk_window_set_child(dialog, layout);

    g_object_set_data_full(G_OBJECT(dialog), "ifm-format-request", req,
        +[](gpointer data) { delete static_cast<Request *>(data); });
    g_signal_connect(req->entry, "changed",
                     G_CALLBACK(on_entry_changed), req);
    g_signal_connect(req->variant, "changed", G_CALLBACK(on_entry_changed), req);
    g_signal_connect(req->format, "notify::selected", G_CALLBACK(on_format_selected), req);
    g_signal_connect(req->confirm, "clicked",
                     G_CALLBACK(on_confirm), dialog);
    gtk_window_present(dialog);
    const char *const list_args[] = {kWorker, "--list-formats", nullptr};
    const char *const type_args[] = {"/usr/bin/lsblk", "--nodeps", "--noheadings", "--raw",
        "--output", "FSTYPE,FSVER", "--", req->target.device.c_str(), nullptr};
    discover(dialog, list_args, true);
    discover(dialog, type_args, false);
    if (req->queries_pending == 0) finish_discovery(req);
}
} // namespace infiltrator::files
