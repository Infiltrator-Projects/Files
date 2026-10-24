// SPDX-License-Identifier: GPL-3.0-or-later
#include "thumbnail_cache.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace infiltrator::files {
namespace {
constexpr goffset kMaximumImageBytes = 32 * 1024 * 1024;
constexpr goffset kMaximumRemoteImageBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaximumCachedTextures = 128;
constexpr std::size_t kMaximumOutstandingDecodes = 4;

void release_weak(GWeakRef *reference)
{
    g_weak_ref_clear(reference);
    delete reference;
}
} // namespace

struct ThumbnailCache::State {
    bool active{true};
    std::unordered_map<std::string, GdkTexture *> ready;
    std::unordered_set<std::string> failed;
    std::unordered_set<std::string> in_flight;
    std::unordered_map<std::string, std::vector<GWeakRef *>> waiters;
    std::deque<std::string> order;
    ~State()
    {
        for (auto &[key, texture] : ready) {
            (void)key;
            g_object_unref(texture);
        }
        for (auto &[key, references] : waiters) {
            (void)key;
            for (GWeakRef *ref : references) release_weak(ref);
        }
    }
};

struct ThumbnailCache::Job {
    std::shared_ptr<State> state;
    std::string key;
    std::string path;
    GFile *file{nullptr};
    int pixel_size{0};
    ~Job() { g_clear_object(&file); }
};

ThumbnailCache::ThumbnailCache() : state_(std::make_shared<State>()) {}
ThumbnailCache::~ThumbnailCache()
{
    state_->active = false;
    for (auto &[key, references] : state_->waiters) {
        (void)key;
        for (GWeakRef *reference : references) release_weak(reference);
    }
    state_->waiters.clear();
    state_.reset();
}

void ThumbnailCache::bind(GtkImage *image, GFileInfo *info, GFile *parent,
                          const int pixel_size)
{
    if (image == nullptr) return;
    // A recycled GTK cell must never receive a completion from its former item.
    g_object_set_data_full(G_OBJECT(image), "ifm-thumb-key", nullptr, nullptr);
    if (info == nullptr || parent == nullptr ||
        (!remote_enabled_ && !g_file_is_native(parent)) ||
        g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR ||
        g_file_info_get_size(info) < 0 ||
        g_file_info_get_size(info) >
            (g_file_is_native(parent) ? kMaximumImageBytes : kMaximumRemoteImageBytes))
        return;
    const char *type = g_file_info_get_content_type(info);
    if (type == nullptr || !g_str_has_prefix(type, "image/")) return;
    const char *name = g_file_info_get_name(info);
    if (name == nullptr || *name == '\0') return;
    GFile *child = g_file_get_child(parent, name);
    char *path = g_file_is_native(child) ? g_file_get_path(child) : nullptr;
    char *uri = g_file_get_uri(child);
    if (uri == nullptr || (g_file_is_native(child) && path == nullptr)) {
        g_free(path); g_free(uri); g_object_unref(child); return;
    }
    const std::string key = std::string(uri) + ":" +
        std::to_string(g_file_info_get_attribute_uint64(
            info, G_FILE_ATTRIBUTE_TIME_MODIFIED)) + ":" +
        std::to_string(g_file_info_get_attribute_uint32(
            info, G_FILE_ATTRIBUTE_TIME_MODIFIED_NSEC)) + ":" +
        std::to_string(g_file_info_get_size(info)) + ":" +
        (g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_ETAG_VALUE) != nullptr
            ? g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_ETAG_VALUE) : "") + ":" +
        std::to_string(pixel_size);
    g_object_set_data_full(G_OBJECT(image), "ifm-thumb-key",
                           g_strdup(key.c_str()), g_free);
    if (const auto found = state_->ready.find(key); found != state_->ready.end()) {
        gtk_image_set_from_paintable(image, GDK_PAINTABLE(found->second));
        g_free(path); g_free(uri); g_object_unref(child);
        return;
    }
    if (state_->failed.find(key) != state_->failed.end() ||
        (state_->in_flight.find(key) == state_->in_flight.end() &&
         state_->in_flight.size() >= kMaximumOutstandingDecodes)) {
        g_free(path); g_free(uri); g_object_unref(child);
        return;
    }
    auto *weak = new GWeakRef;
    g_weak_ref_init(weak, G_OBJECT(image));
    state_->waiters[key].push_back(weak);
    if (!state_->in_flight.insert(key).second) {
        g_free(path); g_free(uri); g_object_unref(child);
        return;
    }

    auto *job = new Job{state_, key, path != nullptr ? path : "",
                        G_FILE(g_object_ref(child)), pixel_size};
    g_free(path); g_free(uri); g_object_unref(child);
    GTask *task = g_task_new(nullptr, nullptr, &ThumbnailCache::finished, nullptr);
    g_task_set_task_data(task, job, [](gpointer p){ delete static_cast<Job *>(p); });
    g_task_run_in_thread(task, &ThumbnailCache::worker);
    g_object_unref(task);
}

void ThumbnailCache::worker(GTask *task, gpointer, gpointer task_data, GCancellable *)
{
    auto *job = static_cast<Job *>(task_data);
    GError *error = nullptr;
    GdkPixbuf *pixels = nullptr;
    if (!job->path.empty()) {
        pixels = gdk_pixbuf_new_from_file_at_scale(
            job->path.c_str(), job->pixel_size, job->pixel_size, TRUE, &error);
    } else {
        // Providers can block or disappear. Bound remote bytes and keep I/O on
        // the worker, so a slow network never stalls first paint or scrolling.
        GFileInputStream *stream = g_file_read(job->file, nullptr, &error);
        if (stream != nullptr) {
            std::vector<guint8> bytes;
            constexpr std::size_t chunk = 64U * 1024U;
            guint8 buffer[chunk];
            while (bytes.size() <= static_cast<std::size_t>(kMaximumRemoteImageBytes)) {
                const gssize count = g_input_stream_read(
                    G_INPUT_STREAM(stream), buffer, chunk, nullptr, &error);
                if (count <= 0) break;
                bytes.insert(bytes.end(), buffer, buffer + count);
            }
            g_input_stream_close(G_INPUT_STREAM(stream), nullptr, nullptr);
            g_object_unref(stream);
            if (error == nullptr && bytes.size() <=
                    static_cast<std::size_t>(kMaximumRemoteImageBytes)) {
                GInputStream *memory = g_memory_input_stream_new_from_data(
                    bytes.data(), static_cast<gssize>(bytes.size()), nullptr);
                pixels = gdk_pixbuf_new_from_stream_at_scale(
                    memory, job->pixel_size, job->pixel_size, TRUE, nullptr, &error);
                g_object_unref(memory);
            } else if (error == nullptr) {
                error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED,
                                            "Image exceeds the remote thumbnail byte limit");
            }
        }
    }
    if (pixels == nullptr) {
        if (error == nullptr)
            error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "Image decode failed");
        g_task_return_error(task, error);
    } else {
        g_task_return_pointer(task, pixels, g_object_unref);
    }
}
void ThumbnailCache::finished(GObject *, GAsyncResult *result, gpointer)
{
    GTask *task = G_TASK(result);
    const auto *job = static_cast<Job *>(g_task_get_task_data(task));
    if (job == nullptr) return;
    std::shared_ptr<State> state = job->state;
    state->in_flight.erase(job->key);
    GError *error = nullptr;
    GdkPixbuf *pixels = static_cast<GdkPixbuf *>(
        g_task_propagate_pointer(task, &error));
    if (!state->active) {
        if (pixels != nullptr) g_object_unref(pixels);
        g_clear_error(&error);
        return;
    }
    GdkTexture *texture = pixels != nullptr ? gdk_texture_new_for_pixbuf(pixels) : nullptr;
    if (pixels != nullptr) g_object_unref(pixels);
    g_clear_error(&error);
    if (texture != nullptr) {
        state->ready[job->key] = texture;
        state->order.push_back(job->key);
        while (state->order.size() > kMaximumCachedTextures) {
            const std::string evicted = state->order.front();
            state->order.pop_front();
            auto old = state->ready.find(evicted);
            if (old != state->ready.end()) {
                g_object_unref(old->second);
                state->ready.erase(old);
            }
        }
    } else {
        state->failed.insert(job->key);
        if (state->failed.size() > kMaximumCachedTextures)
            state->failed.clear();
    }
    auto waiters = state->waiters.find(job->key);
    if (waiters == state->waiters.end()) return;
    for (GWeakRef *ref : waiters->second) {
        GObject *object = static_cast<GObject *>(g_weak_ref_get(ref));
        if (object != nullptr) {
            const char *still = static_cast<const char *>(
                g_object_get_data(object, "ifm-thumb-key"));
            if (texture != nullptr && g_strcmp0(still, job->key.c_str()) == 0) {
                gtk_image_set_from_paintable(GTK_IMAGE(object), GDK_PAINTABLE(texture));
            }
            g_object_unref(object);
        }
        release_weak(ref);
    }
    state->waiters.erase(waiters);
}
} // namespace infiltrator::files
