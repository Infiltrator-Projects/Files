// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_mutation_guard.hpp"

namespace infiltrator::files {

struct DirectoryMutationGuard::State {
    GtkWindow *window;
    GtkDirectoryList *directory;
    GApplication *application{nullptr};
    gulong removed_handler{0U};
    unsigned users{0U};
    bool alive{true};
    bool restore_monitor{false};

    State(GtkWindow *owner, GtkDirectoryList *model) : window(owner), directory(model)
    {
        g_object_ref(directory);
        if (auto *app = gtk_window_get_application(window); app != nullptr) {
            application = G_APPLICATION(g_object_ref(app));
            // gtk_window_destroy removes a window from its application before
            // unrealizing it. GtkWidget::destroy can wait until the last GTask
            // reference is released, so that later signal cannot revoke pending
            // clipboard/chooser authority soon enough on its own.
            removed_handler = g_signal_connect(app, "window-removed",
                G_CALLBACK(+[](GtkApplication *, GtkWindow *removed, gpointer data) {
                    auto *state = static_cast<State *>(data);
                    if (removed == state->window) state->revoke();
                }), this);
        }
    }
    ~State()
    {
        if (application != nullptr) {
            g_signal_handler_disconnect(application, removed_handler);
            g_object_unref(application);
        }
        g_object_unref(directory);
    }
    void revoke()
    {
        alive = false;
        gtk_directory_list_set_monitored(directory, FALSE);
    }
};

DirectoryMutationGuard::DirectoryMutationGuard(GtkWindow *window, GtkDirectoryList *directory)
{
    if (window == nullptr || directory == nullptr) return;
    constexpr const char *key = "ifm-directory-mutation-state";
    auto *existing = static_cast<std::shared_ptr<State> *>(
        g_object_get_data(G_OBJECT(window), key));
    if (existing != nullptr) {
        g_return_if_fail((*existing)->directory == directory);
        state_ = *existing;
        return;
    }
    state_ = std::make_shared<State>(window, directory);
    g_object_set_data_full(G_OBJECT(window), key, new std::shared_ptr<State>(state_),
        [](gpointer data) { delete static_cast<std::shared_ptr<State> *>(data); });
    g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
        auto *state = static_cast<State *>(data);
        state->revoke();
    }), state_.get());
}

DirectoryMutationGuard::~DirectoryMutationGuard() { (void)finish(); }

bool DirectoryMutationGuard::alive() const { return state_ != nullptr && state_->alive; }

void DirectoryMutationGuard::begin()
{
    if (active_ || !alive()) return;
    active_ = true;
    if (state_->users++ == 0U) {
        // A GTask's window reference does not keep GApplication's main loop
        // running after its last visible window closes. Keep the process alive
        // until the final mutation has durably journalled and consumed its result.
        if (state_->application != nullptr) g_application_hold(state_->application);
        state_->restore_monitor = gtk_directory_list_get_monitored(state_->directory);
        gtk_directory_list_set_monitored(state_->directory, FALSE);
    }
}

bool DirectoryMutationGuard::finish()
{
    if (active_) {
        active_ = false;
        if (--state_->users == 0U) {
            const bool restore = state_->restore_monitor;
            state_->restore_monitor = false;
            // GTK starts a fresh enumeration when monitoring is re-enabled.
            // The source GTask may keep a destroyed window alive, but that is
            // not authority to access disposed controls or restart GTK queries.
            if (state_->alive && restore) gtk_directory_list_set_monitored(state_->directory, TRUE);
            if (state_->application != nullptr) g_application_release(state_->application);
        }
    }
    return alive();
}

} // namespace infiltrator::files
