// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/ui/file_manager_window.hpp"
#include "../src/ui/create_folder_controller.hpp"
#include "../src/ui/item_operation_controller.hpp"
#include "../src/ui/directory_mutation_guard.hpp"

#include <atomic>
#include <thread>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace infiltrator::files {
struct FileManagerWindowTestAccess {
    static GtkDirectoryList *directory(FileManagerWindow &window) { return window.directory_list_; }
    static GtkSingleSelection *selection(FileManagerWindow &window) { return window.primary_selection_; }
    static GtkWidget *status(FileManagerWindow &window) { return window.status_label_; }
    static void navigate(FileManagerWindow &window, const std::filesystem::path &path) {
        GFile *file = g_file_new_for_path(path.c_str());
        window.navigate_to(file, true);
        g_object_unref(file);
    }
};
}
using namespace infiltrator::files;
namespace fs = std::filesystem;
namespace {
std::thread::id main_thread;
std::atomic_bool delay_worker_flushes{false};
std::atomic_uint completed_worker_flushes{0U};
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
void await(const std::function<bool()> &ready)
{
    const auto deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!ready() && g_get_monotonic_time() < deadline) {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        g_usleep(1000);
    }
    require(ready(), "Timed out waiting for operation/window disposal");
}
GtkWidget *find(GtkWidget *root, const std::function<bool(GtkWidget *)> &matches)
{
    if (matches(root)) return root;
    for (auto *child = gtk_widget_get_first_child(root); child != nullptr;
         child = gtk_widget_get_next_sibling(child))
        if (auto *result = find(child, matches)) return result;
    return nullptr;
}
FileManagerWindow *window_at(GtkApplication *application, const fs::path &path)
{
    auto *window = new FileManagerWindow(application);
    FileManagerWindowTestAccess::navigate(*window, path);
    window->present();
    await([&] { return !gtk_directory_list_is_loading(FileManagerWindowTestAccess::directory(*window)); });
    return window;
}
void close_and_drain(FileManagerWindow *window)
{
    bool finalized = false;
    g_object_weak_ref(G_OBJECT(window->native_window()), +[](gpointer value, GObject *) {
        *static_cast<bool *>(value) = true;
    }, &finalized);
    gtk_window_destroy(window->native_window());
    await([&] { return finalized; });
    while (g_main_context_iteration(nullptr, FALSE)) {}
}
void create_then_close(GtkApplication *application, const fs::path &folder, const bool file,
                       const bool drain = true)
{
    auto *window = window_at(application, folder);
    new CreateFolderController(window->native_window(), FileManagerWindowTestAccess::directory(*window),
        FileManagerWindowTestAccess::selection(*window), FileManagerWindowTestAccess::status(*window));
    const char *tip = file ? "New Empty File (Ctrl+Alt+N)" : "New Folder (Ctrl+Shift+N)";
    auto *add = find(GTK_WIDGET(window->native_window()), [tip](GtkWidget *widget) {
        return g_strcmp0(gtk_widget_get_tooltip_text(widget), tip) == 0;
    });
    require(add != nullptr, "Create action must exist");
    g_signal_emit_by_name(add, "clicked");
    GListModel *toplevels = gtk_window_get_toplevels();
    GtkWindow *dialog = nullptr;
    for (guint i = 0; i < g_list_model_get_n_items(toplevels); ++i) {
        auto *candidate = GTK_WINDOW(g_list_model_get_item(toplevels, i));
        if (g_strcmp0(gtk_window_get_title(candidate), file ? "New Empty File" : "New Folder") == 0) {
            dialog = candidate; break;
        }
        g_object_unref(candidate);
    }
    require(dialog != nullptr, "Create dialog must open");
    auto *entry = find(GTK_WIDGET(dialog), [](GtkWidget *widget) { return GTK_IS_ENTRY(widget); });
    auto *submit = find(GTK_WIDGET(dialog), [](GtkWidget *widget) {
        return GTK_IS_BUTTON(widget) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), "Create") == 0;
    });
    require(entry != nullptr && submit != nullptr, "Create dialog must expose its fields");
    gtk_editable_set_text(GTK_EDITABLE(entry), file ? "created.txt" : "created-folder");
    g_signal_emit_by_name(submit, "clicked");
    g_object_unref(dialog);
    if (drain) {
        close_and_drain(window); // No main-loop turn is allowed before destruction.
        require(fs::exists(folder / (file ? "created.txt" : "created-folder")),
                "Closing a window must still consume and journal the in-flight result");
    } else {
        gtk_window_destroy(window->native_window());
    }
}
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(const int fd)
{
    const bool worker = delay_worker_flushes.load() && std::this_thread::get_id() != main_thread;
    if (worker) g_usleep(100000);
    const int result = __real_fsync(fd);
    if (worker && result == 0) ++completed_worker_flushes;
    return result;
}
int main()
{
    main_thread = std::this_thread::get_id();
    char *temporary = g_dir_make_tmp("files-operation-lifetime-XXXXXX", nullptr);
    if (temporary == nullptr) return 2;
    const fs::path root(temporary); g_free(temporary);
    g_setenv("XDG_CONFIG_HOME", (root / "config").c_str(), TRUE);
    g_setenv("XDG_STATE_HOME", (root / "state").c_str(), TRUE);
    g_setenv("GTK_A11Y", "none", TRUE);
    g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
    gtk_init();
    auto *application = gtk_application_new("org.infiltrator.Files.OperationLifetimeTest", G_APPLICATION_NON_UNIQUE);
    require(g_application_register(G_APPLICATION(application), nullptr, nullptr), "Cannot register application");
    try {
        for (unsigned i = 0; i < 6U; ++i) {
            const auto folder = root / ("work-" + std::to_string(i));
            fs::create_directory(folder);
            create_then_close(application, folder, (i & 1U) != 0U);
        }
        const auto folder = root / "rename";
        fs::create_directory(folder);
        std::ofstream(folder / "original.txt") << "rename payload";
        auto *window = window_at(application, folder);
        auto *controller = new ItemOperationController(window->native_window(),
            FileManagerWindowTestAccess::directory(*window), FileManagerWindowTestAccess::selection(*window),
            FileManagerWindowTestAccess::status(*window));
        gtk_single_selection_set_selected(FileManagerWindowTestAccess::selection(*window), 0U);
        controller->rename_selected_to("renamed.txt");
        close_and_drain(window);
        require(fs::exists(folder / "renamed.txt"), "Rename must finish safely after window disposal");

        const auto paste_folder = root / "clipboard";
        fs::create_directory(paste_folder);
        window = window_at(application, paste_folder);
        window->install_operation_surface();
        auto *menu = find(gtk_window_get_titlebar(window->native_window()), [](GtkWidget *widget) {
            return GTK_IS_MENU_BUTTON(widget) &&
                   g_strcmp0(gtk_widget_get_tooltip_text(widget), "File actions") == 0;
        });
        require(menu != nullptr, "File actions must exist");
        auto *paste = find(GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(menu))),
            [](GtkWidget *widget) {
                return GTK_IS_BUTTON(widget) &&
                       g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), "Paste") == 0;
            });
        require(paste != nullptr, "Paste action must exist");
        char *source_uri = g_filename_to_uri((folder / "renamed.txt").c_str(), nullptr, nullptr);
        require(source_uri != nullptr, "Clipboard source must have a local URI");
        const std::string offer = std::string("copy\n") + source_uri + "\n";
        g_free(source_uri);
        auto *bytes = g_bytes_new(offer.data(), offer.size());
        auto *provider = gdk_content_provider_new_for_bytes("x-special/gnome-copied-files", bytes);
        auto *clipboard = gdk_display_get_clipboard(
            gtk_widget_get_display(GTK_WIDGET(window->native_window())));
        require(gdk_clipboard_set_content(clipboard, provider), "Structured clipboard offer must succeed");
        g_object_unref(provider);
        g_bytes_unref(bytes);
        g_signal_emit_by_name(paste, "clicked");
        close_and_drain(window); // The stream-open callback is still pending.
        require(!fs::exists(paste_folder / "renamed.txt"),
                "Clipboard completion after window destruction must not start a mutation");

        window = window_at(application, folder);
        auto *directory = FileManagerWindowTestAccess::directory(*window);
        DirectoryMutationGuard first(window->native_window(), directory);
        DirectoryMutationGuard second(window->native_window(), directory);
        first.begin(); second.begin();
        require(!gtk_directory_list_get_monitored(directory), "Concurrent mutations must pause monitoring");
        require(first.finish() && !gtk_directory_list_get_monitored(directory),
                "One completion must not restart monitoring while another mutation is active");
        require(second.finish() && gtk_directory_list_get_monitored(directory),
                "The final completion must restart monitoring and enumerate results");
        await([&] { return !gtk_directory_list_is_loading(directory); });
        first.begin();
        close_and_drain(window);
        require(!first.finish() && !gtk_directory_list_get_monitored(directory),
                "A destroyed window must never restart directory monitoring");

        // Exercise the actual application loop, not a test-owned drain loop.
        // The worker is deliberately still flushing END after the final window
        // is destroyed. GApplication must remain held until completion is consumed.
        auto last_folder = root / "last-window";
        fs::create_directory(last_folder);
        auto *last_application = gtk_application_new("org.infiltrator.Files.LastWindowTest", G_APPLICATION_NON_UNIQUE);
        g_signal_connect(last_application, "activate", G_CALLBACK(+[](GtkApplication *app, gpointer data) {
            create_then_close(app, *static_cast<fs::path *>(data), false, false);
        }), &last_folder);
        delay_worker_flushes.store(true);
        const int status = g_application_run(G_APPLICATION(last_application), 0, nullptr);
        delay_worker_flushes.store(false);
        require(status == 0 && completed_worker_flushes.load() >= 2U,
                "Closing the last window must wait for durable operation completion");
        g_object_unref(last_application);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    g_object_unref(application);
    fs::remove_all(root);
    return 0;
}
