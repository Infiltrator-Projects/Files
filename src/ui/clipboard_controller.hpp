// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "directory_mutation_guard.hpp"
#include "transfer_progress_view.hpp"

#include "../core/batch_transfer_engine.hpp"

#include <gtk/gtk.h>

#include <string>
#include <vector>

namespace infiltrator::files {

class ClipboardController final {
public:
    ClipboardController(GtkWindow *window,
                        GtkDirectoryList *directory_list,
                        GtkMultiSelection *selection,
                        GtkWidget *status_label);
    ~ClipboardController();
    [[nodiscard]] bool transfer_from_drop(const std::vector<std::string> &sources,
                                          const std::string &destination);

    ClipboardController(const ClipboardController &) = delete;
    ClipboardController &operator=(const ClipboardController &) = delete;

private:
    enum class Mode {
        None,
        Copy,
        Cut,
    };

    struct PendingConflict {
        Mode mode{Mode::None};
        std::vector<std::string> sources;
        std::string destination_parent;
    };

    static void on_window_finalized(gpointer user_data, GObject *where_object_was);
    static void on_window_destroyed(GtkWidget *window, gpointer user_data);
    static void on_clipboard_changed(GdkClipboard *clipboard, gpointer user_data);
    static void on_copy_clicked(GtkButton *button, gpointer user_data);
    static void on_cut_clicked(GtkButton *button, gpointer user_data);
    static void on_paste_clicked(GtkButton *button, gpointer user_data);
    static gboolean on_key_pressed(GtkEventControllerKey *controller,
                                   guint keyval,
                                   guint keycode,
                                   GdkModifierType state,
                                   gpointer user_data);
    static void on_selection_changed(GtkSelectionModel *model,
                                     guint position,
                                     guint n_items,
                                     gpointer user_data);
    static void on_location_changed(GObject *object, GParamSpec *pspec, gpointer user_data);
    static void on_clipboard_stream_open(GObject *source_object,
                                         GAsyncResult *result, gpointer user_data);
    static void on_clipboard_stream_read(GObject *source_object,
                                         GAsyncResult *result, gpointer user_data);
    static void on_clipboard_text_read(GObject *source_object,
                                       GAsyncResult *result,
                                       gpointer user_data);
    static void on_operation_thread(GTask *task,
                                    gpointer source_object,
                                    gpointer task_data,
                                    GCancellable *cancellable);
    static void on_operation_finished(GObject *source_object,
                                      GAsyncResult *result,
                                      gpointer user_data);
    static void on_conflict_chosen(GObject *source_object,
                                   GAsyncResult *result,
                                   gpointer user_data);

    void install_menu_actions();
    void install_keyboard_shortcuts();
    void capture_selection(Mode mode);
    void paste();
    void paste_sources(Mode mode,
                       const std::vector<std::string> &sources,
                       const std::string &destination_parent,
                       BatchConflictPolicy policy);
    void prompt_conflict(Mode mode,
                         const std::vector<std::string> &sources,
                         const std::string &destination_parent,
                         const std::string &detail);
    void publish_clipboard_text();
    void refresh_cut_state_after_paste(const std::vector<std::string> &sources);
    void update_action_state();
    void set_status(const std::string &message) const;
    void show_alert(const char *title, const std::string &detail) const;

    [[nodiscard]] std::vector<std::string> selected_sources() const;
    [[nodiscard]] std::string current_destination() const;
    [[nodiscard]] std::vector<std::string> parse_local_file_references(const char *text) const;

    DirectoryMutationGuard mutation_guard_;
    TransferProgressView progress_view_;
    GtkWindow *window_{nullptr};
    GtkDirectoryList *directory_list_{nullptr};
    GtkMultiSelection *selection_{nullptr};
    GtkWidget *status_label_{nullptr};
    GtkWidget *copy_button_{nullptr};
    GtkWidget *cut_button_{nullptr};
    GtkWidget *paste_button_{nullptr};
    GdkClipboard *clipboard_{nullptr};
    GdkContentProvider *owned_content_{nullptr};
    gulong clipboard_changed_handler_{0U};

    Mode mode_{Mode::None};
    std::vector<std::string> sources_;
    PendingConflict pending_conflict_{};
    bool busy_{false};
    bool reading_external_clipboard_{false};
    // Immutable destination selected when an external Paste request starts.
    // Navigation during asynchronous clipboard I/O must not retarget mutation.
    std::string clipboard_requested_destination_;
    guint64 clipboard_generation_{0U};
    guint64 clipboard_read_generation_{0U};
    gulong selection_handler_{0U};
    gulong location_handler_{0U};
};

} // namespace infiltrator::files
