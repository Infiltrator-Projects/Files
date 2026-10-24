// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_manager_window.hpp"

#include "archive_controller.hpp"
#include "batch_item_operation_controller.hpp"
#include "clipboard_controller.hpp"
#include "create_folder_controller.hpp"
#include "destructive_operation_controller.hpp"
#include "drag_drop_controller.hpp"
#include "dual_pane_controller.hpp"
#include "interaction_controller.hpp"
#include "item_operation_controller.hpp"
#include "journal_recovery_controller.hpp"

namespace infiltrator::files {

void FileManagerWindow::install_operation_surface()
{
    if (operation_surface_installed_) {
        return;
    }

    // Controllers own distinct mutation concerns and tie their lifetime to the
    // native window. Keeping construction here prevents those concrete
    // controller dependencies from leaking through FileManagerWindow's header.
    new CreateFolderController(GTK_WINDOW(window_), directory_list_, primary_selection_, status_label_);
    new ItemOperationController(GTK_WINDOW(window_), directory_list_, primary_selection_, status_label_);
    new DestructiveOperationController(GTK_WINDOW(window_), directory_list_, primary_selection_, status_label_, selection_);
    new BatchItemOperationController(GTK_WINDOW(window_), directory_list_, selection_, status_label_);
    new JournalRecoveryController(GTK_WINDOW(window_), status_label_);
    new ArchiveController(GTK_WINDOW(window_), directory_list_, primary_selection_, status_label_);

    // Install the shared interaction layer after the mutation controllers. It
    // consolidates their already-qualified actions rather than reimplementing them.
    new InteractionController(GTK_WINDOW(window_),
                              directory_list_,
                              selection_,
                              primary_selection_,
                              location_entry_,
                              content_stack_,
                              status_label_);

    // Clipboard operations deliberately reuse BatchTransferEngine, including its
    // per-item journalling, collision preflight and deterministic recovery rules.
    // It is installed last so its Copy/Cut/Paste actions join the consolidated
    // File actions surface created immediately above.
    new ClipboardController(GTK_WINDOW(window_), directory_list_, selection_, status_label_);
    if (GtkWidget *host = GTK_WIDGET(
            g_object_get_data(G_OBJECT(window_), "ifm-secondary-pane-host"));
        host != nullptr)
        new DualPaneController(GTK_WINDOW(window_), host, directory_list_,
                               selection_, status_label_);
    new DragDropController(GTK_WINDOW(window_), directory_list_, selection_, content_stack_, sidebar_);

    operation_surface_installed_ = true;
}

} // namespace infiltrator::files
