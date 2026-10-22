// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <gtk/gtk.h>
#include <memory>

namespace infiltrator::files {

// UI-thread lease over the window's one monitored directory model. Mutations
// from different controllers share a depth count: only the last completion may
// restart monitoring/reload, and a destroyed window can never restart it.
class DirectoryMutationGuard final {
public:
    DirectoryMutationGuard(GtkWindow *window, GtkDirectoryList *directory);
    ~DirectoryMutationGuard();
    DirectoryMutationGuard(const DirectoryMutationGuard &) = delete;
    DirectoryMutationGuard &operator=(const DirectoryMutationGuard &) = delete;

    void begin();
    [[nodiscard]] bool finish();
    [[nodiscard]] bool alive() const;

private:
    struct State;
    std::shared_ptr<State> state_;
    bool active_{false};
};

} // namespace infiltrator::files
