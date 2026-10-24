// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <gtk/gtk.h>
#include <string>
#include <vector>

namespace infiltrator::files {

// The secondary browser owns only presentation. All cross-pane mutations
// are submitted to ClipboardController's journalled BatchTransferEngine.
class DualPaneController final {
public:
    DualPaneController(GtkWindow *window, GtkWidget *host,
                       GtkDirectoryList *primary, GtkMultiSelection *primary_selection,
                       GtkWidget *status);
    ~DualPaneController();
    DualPaneController(const DualPaneController &) = delete;
    DualPaneController &operator=(const DualPaneController &) = delete;
    void toggle();
    [[nodiscard]] GFile *secondary_location() const noexcept {
        return secondary_ != nullptr ? gtk_directory_list_get_file(secondary_) : nullptr;
    }
    [[nodiscard]] bool visible() const noexcept;
private:
    static void on_window_finalized(gpointer, GObject *);
    static void setup_row(GtkSignalListItemFactory *, GtkListItem *, gpointer);
    static void bind_row(GtkSignalListItemFactory *, GtkListItem *, gpointer);
    static void on_activate(GtkListView *, guint, gpointer);
    static void on_go(GtkEntry *, gpointer);
    static void on_up(GtkButton *, gpointer);
    static void on_back(GtkButton *, gpointer);
    static void on_forward(GtkButton *, gpointer);
    static void on_select_all(GtkButton *, gpointer);
    static void on_transfer(GtkButton *, gpointer);
    static void on_cancel_import(GtkButton *, gpointer);
    static void on_loading(GObject *, GParamSpec *, gpointer);
    void navigate(GFile *);
    void transfer(bool to_secondary, bool move);
    [[nodiscard]] std::vector<std::string> selected_sources(bool from_primary) const;
    [[nodiscard]] std::vector<std::string> selected_provider_uris() const;
    void status(const char *) const;
    GtkWindow *window_{nullptr}; // weak; window finalization deletes controller
    GtkWidget *host_{nullptr};
    GtkWidget *path_{nullptr};
    GtkWidget *back_{nullptr};
    GtkWidget *forward_{nullptr};
    GtkWidget *feedback_{nullptr};
    GtkDirectoryList *primary_{nullptr};
    GtkMultiSelection *primary_selection_{nullptr};
    GtkDirectoryList *secondary_{nullptr};
    GtkMultiSelection *secondary_selection_{nullptr};
    GtkWidget *status_label_{nullptr};
    std::vector<std::string> history_;
    std::vector<std::string> forward_history_;
};

} // namespace infiltrator::files
