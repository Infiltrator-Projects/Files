// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_manager_window.hpp"

#include "batch_item_operation_controller.hpp"
#include "create_folder_controller.hpp"
#include "destructive_operation_controller.hpp"
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
    new DestructiveOperationController(GTK_WINDOW(window_), directory_list_, primary_selection_, status_label_);
    new BatchItemOperationController(GTK_WINDOW(window_), directory_list_, selection_, status_label_);
    new JournalRecoveryController(GTK_WINDOW(window_), status_label_);
    operation_surface_installed_ = true;
}

} // namespace infiltrator::files
