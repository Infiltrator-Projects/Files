// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../core/operation_journal.hpp"

#include <gtk/gtk.h>

#include <vector>

namespace infiltrator::files {

class JournalRecoveryController final {
public:
    JournalRecoveryController(GtkWindow *window, GtkWidget *status_label);
    ~JournalRecoveryController() = default;

    JournalRecoveryController(const JournalRecoveryController &) = delete;
    JournalRecoveryController &operator=(const JournalRecoveryController &) = delete;

private:
    static void on_window_finalized(gpointer user_data, GObject *where_object_was);
    static void on_review_clicked(GtkButton *button, gpointer user_data);

    void show_review() const;

    GtkWindow *window_{nullptr};
    GtkWidget *status_label_{nullptr};
    GtkWidget *review_button_{nullptr};
    std::vector<InterruptedOperation> interrupted_;
    bool journal_healthy_{true};
};

} // namespace infiltrator::files
