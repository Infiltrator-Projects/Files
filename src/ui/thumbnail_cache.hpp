// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <gtk/gtk.h>
#include <memory>

namespace infiltrator::files {

// Nonblocking, bounded image thumbnail cache shared by all Files presentations.
// Workers decode pixbufs only; GTK textures and widgets are touched on the main loop.
class ThumbnailCache final {
public:
    ThumbnailCache();
    ~ThumbnailCache();
    ThumbnailCache(const ThumbnailCache &) = delete;
    ThumbnailCache &operator=(const ThumbnailCache &) = delete;
    void bind(GtkImage *image, GFileInfo *info, GFile *parent, int pixel_size);
private:
    struct State;
    std::shared_ptr<State> state_;
    struct Job;
    static void worker(GTask *task, gpointer source, gpointer task_data, GCancellable *);
    static void finished(GObject *source, GAsyncResult *result, gpointer user_data);
};
} // namespace infiltrator::files
