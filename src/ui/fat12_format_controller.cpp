// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * FAT12 pilot: Files is a selection, confirmation and unmount GUI only.
 * The separately installed FAT12.so is the sole formatting engine. A
 * root-owned helper supplies verified partition-relative block I/O.
 */
#include "fat12_format_controller.hpp"
#include "../core/physical_media.h"
#include <gio/gio.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <cstring>
#include <string>
#include <utility>

namespace infiltrator::files {
namespace {
constexpr const char *kWorker = "/System/Commands/files-fat12-format";
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
struct Request {
    Target target;
    GtkWidget *entry{nullptr};
    GtkWidget *status{nullptr};
    GtkWidget *confirm{nullptr};
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
        g_object_unref(system_root);
        if (root != nullptr) g_object_unref(root);
        if (!is_root) {
            GVolume *volume = g_mount_get_volume(mount);
            if (volume != nullptr) {
                GDrive *drive = g_volume_get_drive(volume);
                char *device = g_volume_get_identifier(
                    volume, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
                struct stat st{};
                if (device != nullptr &&
                    std::strncmp(device, "/dev/", 5) == 0 &&
                    std::strchr(device + 5, '/') == nullptr &&
                    lstat(device, &st) == 0 && S_ISBLK(st.st_mode) &&
                    ifm_partition_is_physical_removable(
                        static_cast<unsigned>(major(st.st_rdev)),
                        static_cast<unsigned>(minor(st.st_rdev)))) {
                    out.device = device;
                    // Retain the volume across unmount so an abort before
                    // formatting starts can safely restore its mount.
                    out.volume = G_VOLUME(g_object_ref(volume));
                    out.major_number = static_cast<unsigned>(major(st.st_rdev));
                    out.minor_number = static_cast<unsigned>(minor(st.st_rdev));
                    out.mount_name = "the selected removable volume";
                    if (char *name = g_mount_get_name(mount); name != nullptr) {
                        out.mount_name = name;
                        g_free(name);
                    }
                    valid = true;
                }
                g_free(device);
                if (drive != nullptr) g_object_unref(drive);
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
void complete(GtkWindow *dialog, bool success, const char *message)
{
    Request *req = static_cast<Request *>(
        g_object_get_data(G_OBJECT(dialog), "ifm-format-request"));
    set_status(req, message);
    gtk_window_set_deletable(dialog, TRUE);
    if (req != nullptr && !success) {
        gtk_widget_set_sensitive(req->confirm, TRUE);
        gtk_widget_set_sensitive(req->entry, TRUE);
    }
}
/* Remount only when the formatter definitely did not execute. A failed
 * formatter may have modified on-disk metadata and must remain unmounted. */
void on_safe_remount(GObject *object, GAsyncResult *result, gpointer data)
{
    GtkWindow *dialog = GTK_WINDOW(data);
    Request *req = static_cast<Request *>(
        g_object_get_data(G_OBJECT(dialog), "ifm-format-request"));
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
    complete(dialog, false, message.c_str());
    g_clear_error(&error);
    g_object_unref(dialog);
}

void restore_after_preformat_failure(GtkWindow *dialog,
                                     const std::string& reason)
{
    Request *req = static_cast<Request *>(
        g_object_get_data(G_OBJECT(dialog), "ifm-format-request"));
    if (req == nullptr || req->target.volume == nullptr) {
        const std::string message = reason +
            "\nThe volume remains unmounted; the formatter did not start.";
        complete(dialog, false, message.c_str());
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
        complete(dialog, true,
                 "FAT12 formatting succeeded. The volume can now be remounted.");
    } else {
        const char *reason = error != nullptr ? error->message :
            err != nullptr && *err ? err : "FAT12 format host failed.";
        const std::string message = std::string("Formatting failed: ") + reason;
        GSubprocess *process = G_SUBPROCESS(object);
        // pkexec uses 126/127 if authorisation or execution was refused.
        // Our host returns only 0, 1 or 2, so no format writes occurred.
        const bool never_started = completed &&
            g_subprocess_get_if_exited(process) &&
            (g_subprocess_get_exit_status(process) == 126 ||
             g_subprocess_get_exit_status(process) == 127);
        if (never_started) {
            restore_after_preformat_failure(dialog, message);
        } else {
            const std::string warning = message +
                "\nThe volume remains unmounted because formatting may have "
                "modified filesystem metadata.";
            complete(dialog, false, warning.c_str());
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
    Request *req = static_cast<Request *>(
        g_object_get_data(G_OBJECT(dialog), "ifm-format-request"));
    GError *error = nullptr;
    if (!g_mount_unmount_with_operation_finish(G_MOUNT(object), result, &error)) {
        std::string message = std::string("Could not unmount volume: ") +
            (error != nullptr ? error->message : "Unknown error");
        complete(dialog, false, message.c_str());
        g_clear_error(&error);
        g_object_unref(dialog);
        return;
    }
    const std::string major_number = std::to_string(req->target.major_number);
    const std::string minor_number = std::to_string(req->target.minor_number);
    const char *const argv[] = {
        "pkexec", kWorker, "--format-fat12", req->target.device.c_str(),
        major_number.c_str(), minor_number.c_str(), nullptr
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
    set_status(req, "Volume unmounted. Awaiting authentication and FAT12 formatting.");
    g_subprocess_communicate_utf8_async(
        process, nullptr, nullptr, on_process_done, g_object_ref(dialog));
    g_object_unref(process);
    g_object_unref(dialog);
}
void on_confirm(GtkButton *, gpointer data)
{
    GtkWindow *dialog = GTK_WINDOW(data);
    Request *req = static_cast<Request *>(
        g_object_get_data(G_OBJECT(dialog), "ifm-format-request"));
    if (req == nullptr ||
        std::strcmp(gtk_editable_get_text(GTK_EDITABLE(req->entry)), "FORMAT") != 0)
        return;
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
void on_entry_changed(GtkEditable *entry, gpointer data)
{
    Request *req = static_cast<Request *>(data);
    gtk_widget_set_sensitive(req->confirm,
        std::strcmp(gtk_editable_get_text(entry), "FORMAT") == 0);
}
} // namespace

bool fat12_format_available(GFile *folder)
{
    Target target;
    return find_target(folder, target);
}
void show_fat12_format_dialog(GtkWindow *parent, GFile *folder)
{
    if (!GTK_IS_WINDOW(parent)) return;
    auto *req = new Request;
    if (!find_target(folder, req->target)) {
        delete req;
        return;
    }
    GtkWindow *dialog = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(dialog, "Format containing volume as FAT12");
    gtk_window_set_transient_for(dialog, parent);
    gtk_window_set_modal(dialog, TRUE);
    gtk_window_set_default_size(dialog, 560, -1);
    GtkWidget *layout = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(layout, 16);
    gtk_widget_set_margin_end(layout, 16);
    gtk_widget_set_margin_top(layout, 16);
    gtk_widget_set_margin_bottom(layout, 16);
    std::string message =
        "This will erase ALL files on the entire removable volume:\n\n" +
        req->target.mount_name + "\n" + req->target.device +
        "\n\nThe selected folder is NOT formatted separately. "
        "The containing filesystem is unmounted, then the FAT12.so module "
        "formats the partition. This cannot be undone.";
    GtkWidget *warning = gtk_label_new(message.c_str());
    gtk_label_set_wrap(GTK_LABEL(warning), TRUE);
    gtk_label_set_xalign(GTK_LABEL(warning), 0.0F);
    gtk_box_append(GTK_BOX(layout), warning);

    req->entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(req->entry), "Type FORMAT to confirm");
    gtk_box_append(GTK_BOX(layout), req->entry);
    req->confirm = gtk_button_new_with_label("Erase volume and format FAT12");
    gtk_widget_set_sensitive(req->confirm, FALSE);
    gtk_box_append(GTK_BOX(layout), req->confirm);
    req->status = gtk_label_new("The volume will not be changed until confirmed.");
    gtk_label_set_wrap(GTK_LABEL(req->status), TRUE);
    gtk_label_set_xalign(GTK_LABEL(req->status), 0.0F);
    gtk_box_append(GTK_BOX(layout), req->status);
    gtk_window_set_child(dialog, layout);

    g_object_set_data_full(G_OBJECT(dialog), "ifm-format-request", req,
        +[](gpointer data) { delete static_cast<Request *>(data); });
    g_signal_connect(req->entry, "changed",
                     G_CALLBACK(on_entry_changed), req);
    g_signal_connect(req->confirm, "clicked",
                     G_CALLBACK(on_confirm), dialog);
    gtk_window_present(dialog);
}
} // namespace infiltrator::files
