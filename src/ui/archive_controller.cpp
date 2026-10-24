// SPDX-License-Identifier: GPL-3.0-or-later
#include "archive_controller.hpp"
#include "../core/archive_engine.hpp"
#include "../core/operation_journal.hpp"
#include <memory>
#include <string>
namespace infiltrator::files {
namespace {
struct Request {
    std::string source;
    std::string destination;
    bool create{false};
};
}
ArchiveController::ArchiveController(GtkWindow *window, GtkDirectoryList *directory,
                                     GtkSingleSelection *selection, GtkWidget *status)
    : guard_(window, directory), window_(window), directory_(directory),
      selection_(selection), status_(status)
{
    g_object_ref(directory_);
    g_object_ref(selection_);
    g_object_set_data(G_OBJECT(window_), "ifm-archive-controller", this);
    g_signal_connect(window_, "destroy", G_CALLBACK(on_window_destroyed), this);
    g_object_weak_ref(G_OBJECT(window_), on_window_finalized, this);
}
ArchiveController::~ArchiveController()
{
    if (cancellable_ != nullptr) { g_cancellable_cancel(cancellable_); g_clear_object(&cancellable_); }
    g_clear_object(&selection_);
    g_clear_object(&directory_);
    if (window_ != nullptr) g_object_set_data(G_OBJECT(window_), "ifm-archive-controller", nullptr);
}
void ArchiveController::on_window_destroyed(GtkWidget *, gpointer data)
{
    auto *self = static_cast<ArchiveController *>(data);
    if (self->cancellable_ != nullptr) g_cancellable_cancel(self->cancellable_);
    self->status_ = nullptr;
}
void ArchiveController::on_window_finalized(gpointer data, GObject *)
{
    auto *self = static_cast<ArchiveController *>(data);
    self->window_ = nullptr;
    delete self;
}
void ArchiveController::extract_selected() { start_selected(false); }
void ArchiveController::create_selected() { start_selected(true); }
void ArchiveController::start_selected(bool create)
{
    if (busy_ || window_ == nullptr || directory_ == nullptr) return;
    GFile *parent = gtk_directory_list_get_file(directory_);
    if (parent == nullptr || !g_file_is_native(parent)) return;
    auto *info = G_FILE_INFO(gtk_single_selection_get_selected_item(selection_));
    if (info == nullptr) return;
    const char *name = g_file_info_get_name(info);
    if (name == nullptr || g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR) {
        g_object_unref(info); return;
    }
    GFile *file = g_file_get_child(parent, name);
    char *source = g_file_get_path(file);
    char *destination = g_file_get_path(parent);
    g_object_unref(file);
    g_object_unref(info);
    if (source == nullptr || destination == nullptr || !guard_.begin()) {
        g_free(source); g_free(destination); return;
    }
    g_clear_object(&cancellable_);
    cancellable_ = g_cancellable_new();
    auto *request = new Request{source, destination, create};
    g_free(source); g_free(destination);
    busy_ = true;
    if (status_ != nullptr)
        gtk_label_set_text(GTK_LABEL(status_), create ? "Creating ZIP archive safely…" : "Extracting selected archive safely…");
    GTask *task = g_task_new(G_OBJECT(window_), cancellable_, on_finished, this);
    g_task_set_task_data(task, request, [](gpointer value) {
        delete static_cast<Request *>(value);
    });
    g_task_set_check_cancellable(task, FALSE);
    g_task_run_in_thread(task, on_thread);
    g_object_unref(task);
}
void ArchiveController::on_thread(GTask *task, gpointer, gpointer user_data,
                                  GCancellable *cancellable)
{
    const auto *request = static_cast<Request *>(user_data);
    OperationJournal journal;
    const char *kind = request->create ? "CreateArchive" : "ExtractArchive";
    const std::string id = journal.begin(kind, request->source,
                                          request->destination);
    OperationResult result;
    if (id.empty()) {
        result = {OperationStatus::VerificationFailure, OperationPhase::Preflight,
                  request->destination, "Extraction refused: durable journal unavailable.", false, {}};
    } else {
        ArchiveEngine engine;
        result = request->create
            ? engine.create_zip(request->source, request->destination, id, cancellable)
            : engine.extract(request->source, request->destination, id, cancellable);
        if (!journal.finish(id, kind, result)) {
            result.status = OperationStatus::VerificationFailure;
            result.phase = OperationPhase::Verify;
            result.message += " The operation journal could not record completion.";
        }
    }
    g_task_return_pointer(task, new OperationResult(std::move(result)), [](gpointer value) {
        delete static_cast<OperationResult *>(value);
    });
}
void ArchiveController::on_finished(GObject *, GAsyncResult *result, gpointer data)
{
    auto *self = static_cast<ArchiveController *>(data);
    GError *error = nullptr;
    std::unique_ptr<OperationResult> operation(
        static_cast<OperationResult *>(g_task_propagate_pointer(G_TASK(result), &error)));
    g_clear_object(&self->cancellable_);
    if (!self->guard_.finish()) { g_clear_error(&error); return; }
    self->busy_ = false;
    std::string text = error != nullptr ? error->message :
        operation != nullptr ? operation->message : "Archive operation failed.";
    if (self->status_ != nullptr) gtk_label_set_text(GTK_LABEL(self->status_), text.c_str());
    if ((error != nullptr || (operation != nullptr && !operation->ok())) &&
        self->window_ != nullptr) {
        GtkAlertDialog *dialog = gtk_alert_dialog_new("Archive operation incomplete");
        gtk_alert_dialog_set_detail(dialog, text.c_str());
        gtk_alert_dialog_show(dialog, self->window_);
        g_object_unref(dialog);
    }
    g_clear_error(&error);
}
} // namespace infiltrator::files
