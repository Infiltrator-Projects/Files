// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "directory_mutation_guard.hpp"
#include "../core/transfer_operation_engine.hpp"
#include <memory>

namespace infiltrator::files {

// One presentation for the engine's progress/cancellation contract. It owns
// neither the worker nor mutation admission; the controller keeps those rules.
class TransferProgressView final {
public:
    TransferProgressView(GtkWindow *window, DirectoryMutationGuard &lifetime,
                         GtkWidget *status, const char *description);
    ~TransferProgressView();
    TransferProgressView(const TransferProgressView &) = delete;
    TransferProgressView &operator=(const TransferProgressView &) = delete;
    void start(std::shared_ptr<TransferControl> control);
    void finish();
private:
    static gboolean tick(gpointer data);
    static void cancel(GtkButton *, gpointer data);
    void refresh();
    DirectoryMutationGuard &lifetime_;
    GtkWidget *status_{nullptr};
    GtkWidget *box_{nullptr};
    GtkWidget *bar_{nullptr};
    GtkWidget *cancel_{nullptr};
    guint timer_{0U};
    std::shared_ptr<TransferControl> control_;
};
} // namespace infiltrator::files
