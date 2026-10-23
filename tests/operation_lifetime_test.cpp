// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/ui/file_manager_window.hpp"
#include "../src/ui/create_folder_controller.hpp"
#include "../src/ui/item_operation_controller.hpp"
#include "../src/ui/directory_mutation_guard.hpp"
#include "../src/ui/clipboard_controller.hpp"

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
    static GtkMultiSelection *multi(FileManagerWindow &window) { return window.selection_; }
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
std::atomic_uint started_worker_flushes{0U};
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
    if (root == nullptr) return nullptr;
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
GtkWidget *paste_action(FileManagerWindow &window)
{
    auto *menu = find(gtk_window_get_titlebar(window.native_window()), [](GtkWidget *widget) {
        return GTK_IS_MENU_BUTTON(widget) &&
               g_strcmp0(gtk_widget_get_tooltip_text(widget), "File actions") == 0;
    });
    require(menu != nullptr, "File actions must exist");
    auto *paste = find(GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(menu))),
        [](GtkWidget *widget) {
            return GTK_IS_BUTTON(widget) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), "Paste") == 0;
        });
    require(paste != nullptr, "Paste action must exist");
    return paste;
}
void offer_files(FileManagerWindow &window, const std::vector<fs::path> &sources, const bool cut)
{
    std::string offer = cut ? "cut\n" : "copy\n";
    for (const auto &source : sources) {
        char *uri = g_filename_to_uri(source.c_str(), nullptr, nullptr);
        require(uri != nullptr, "Clipboard source must have a local URI");
        offer += uri; offer += '\n'; g_free(uri);
    }
    auto *bytes = g_bytes_new(offer.data(), offer.size());
    auto *provider = gdk_content_provider_new_for_bytes("x-special/gnome-copied-files", bytes);
    auto *clipboard = gdk_display_get_clipboard(gtk_widget_get_display(GTK_WIDGET(window.native_window())));
    require(gdk_clipboard_set_content(clipboard, provider), "Structured clipboard offer must succeed");
    g_object_unref(provider); g_bytes_unref(bytes);
}
void removal_batch(GtkApplication *application, const fs::path &root, const bool trash,
                   const bool confirm, const bool close_before_answer)
{
    fs::create_directories(root);
    std::ofstream(root / "first.txt") << "FIRST";
    std::ofstream(root / "second.txt") << "SECOND";
    auto *window = window_at(application, root);
    window->install_operation_surface();
    gtk_selection_model_select_all(GTK_SELECTION_MODEL(FileManagerWindowTestAccess::multi(*window)));
    auto *menu = find(gtk_window_get_titlebar(window->native_window()), [](GtkWidget *widget) {
        return GTK_IS_MENU_BUTTON(widget) && g_strcmp0(gtk_widget_get_tooltip_text(widget), "File actions") == 0;
    });
    const char *verb = trash ? "Move to Trash" : "Delete Permanently…";
    auto *action = find(GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(menu))), [verb](GtkWidget *widget) {
        return GTK_IS_BUTTON(widget) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), verb) == 0;
    });
    require(action != nullptr && gtk_widget_get_visible(action) && gtk_widget_get_sensitive(action),
            "Trash and permanent delete must be available for a multiple selection");
    g_signal_emit_by_name(action, "clicked");
    if (!trash) {
        GtkWindow *dialog = nullptr;
        GtkWidget *answer = nullptr;
        await([&] {
            auto *toplevels = gtk_window_get_toplevels();
            for (guint i = 0; i < g_list_model_get_n_items(toplevels); ++i) {
                auto *candidate = GTK_WINDOW(g_list_model_get_item(toplevels, i));
                auto *button = find(GTK_WIDGET(candidate), [confirm](GtkWidget *widget) {
                    return GTK_IS_BUTTON(widget) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)),
                        confirm ? "Delete Permanently" : "Cancel") == 0;
                });
                if (button != nullptr) { dialog = candidate; answer = button; return true; }
                g_object_unref(candidate);
            }
            return false;
        });
        require(fs::exists(root / "first.txt") && fs::exists(root / "second.txt"),
                "No item may be deleted before explicit confirmation");
        g_signal_emit_by_name(answer, "clicked");
        g_object_unref(dialog);
        if (!close_before_answer) {
            if (confirm) await([&] { return !fs::exists(root / "first.txt") && !fs::exists(root / "second.txt"); });
            else while (g_main_context_iteration(nullptr, FALSE)) {}
        }
    }
    close_and_drain(window);
    const bool removed = trash || (confirm && !close_before_answer);
    require(fs::exists(root / "first.txt") != removed && fs::exists(root / "second.txt") != removed,
            "A batch must honour its confirmation decision, and late decisions must be refused");
}
void conflict_completion(GtkApplication *application, const fs::path &root,
                         const char *choice, const bool cut, const bool close_early,
                         const bool close_during_work = false)
{
    const auto source = root / "source", destination = root / "destination";
    fs::create_directories(source); fs::create_directories(destination);
    std::ofstream(source / "same.txt") << "NEW";
    std::ofstream(source / "other.txt") << "OTHER";
    std::ofstream(destination / "same.txt") << "OLD";
    auto *window = window_at(application, destination);
    window->install_operation_surface();
    auto *paste = paste_action(*window);
    offer_files(*window, {source / "same.txt", source / "other.txt"}, cut);
    g_signal_emit_by_name(paste, "clicked");
    GtkWindow *dialog = nullptr;
    GtkWidget *answer = nullptr;
    await([&] {
        auto *toplevels = gtk_window_get_toplevels();
        for (guint i = 0; i < g_list_model_get_n_items(toplevels); ++i) {
            auto *candidate = GTK_WINDOW(g_list_model_get_item(toplevels, i));
            auto *button = find(GTK_WIDGET(candidate), [choice](GtkWidget *widget) {
                return GTK_IS_BUTTON(widget) &&
                       g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), choice) == 0;
            });
            if (button != nullptr) { dialog = candidate; answer = button; return true; }
            g_object_unref(candidate);
        }
        return false;
    });
    g_signal_emit_by_name(answer, "clicked");
    g_object_unref(dialog);
    if (close_during_work) {
        // Let the live conflict continuation acquire its lease and enter a
        // delayed worker flush, then close the actual last application window.
        auto *native = GTK_WINDOW(g_object_ref(window->native_window()));
        g_timeout_add_full(G_PRIORITY_DEFAULT, 1, +[](gpointer value) -> gboolean {
            if (started_worker_flushes.load() == 0U) return G_SOURCE_CONTINUE;
            gtk_window_destroy(GTK_WINDOW(value));
            return G_SOURCE_REMOVE;
        }, native, g_object_unref);
        return;
    }
    if (!close_early)
        await([&] { return fs::exists(destination / "other.txt") && gtk_widget_get_sensitive(paste); });
    close_and_drain(window);
    std::ifstream original(destination / "same.txt");
    const std::string contents{std::istreambuf_iterator<char>(original), {}};
    if (close_early) {
        require(fs::exists(source / "same.txt") && fs::exists(source / "other.txt") &&
                contents == "OLD" && !fs::exists(destination / "other.txt") &&
                !fs::exists(destination / "same (copy).txt"),
                "A late clipboard conflict choice must never create, replace or move files");
    } else {
        require(contents == (std::string(choice) == "Replace Conflicts" ? "NEW" : "OLD"),
                "Live clipboard conflict resolution must retain its chosen policy");
        require(!cut || !fs::exists(source / "other.txt"), "A live Cut continuation must complete its move");
    }
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
    if (worker) { ++started_worker_flushes; g_usleep(100000); }
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
    fs::create_directories(root / "data");
    g_setenv("XDG_DATA_HOME", (root / "data").c_str(), TRUE);
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

        unsigned conflict_index = 0U;
        for (const bool cut : {false, true}) {
            for (const char *choice : {"Skip Conflicts", "Keep Both", "Replace Conflicts"}) {
                for (const bool close_early : {false, true})
                    conflict_completion(application, root / ("conflict-" + std::to_string(conflict_index++)),
                                        choice, cut, close_early);
            }
        }

        const auto drop_source = root / "drop-source";
        const auto drop_destination = root / "drop-destination";
        fs::create_directories(drop_source);
        fs::create_directories(drop_destination);
        std::ofstream(drop_source / "large.dat") << "payload";
        fs::resize_file(drop_source / "large.dat", 64U * 1024U * 1024U);
        window = window_at(application, drop_destination);
        window->install_operation_surface();
        auto *clipboard_controller = static_cast<ClipboardController *>(
            g_object_get_data(G_OBJECT(window->native_window()), "ifm-clipboard-controller"));
        require(clipboard_controller != nullptr && clipboard_controller->transfer_from_drop(
                    {(drop_source / "large.dat").string()}, drop_destination.string()),
                "Drag and drop must start the shared clipboard transfer");
        auto *progress = find(gtk_window_get_titlebar(window->native_window()), [](GtkWidget *widget) {
            return GTK_IS_PROGRESS_BAR(widget) &&
                g_strcmp0(gtk_widget_get_tooltip_text(widget), "Clipboard transfer progress") == 0;
        });
        require(progress != nullptr && gtk_widget_get_visible(gtk_widget_get_parent(progress)),
                "Paste and drag transfers must display progress immediately");
        auto *cancel = find(gtk_widget_get_parent(progress), [](GtkWidget *widget) {
            return GTK_IS_BUTTON(widget) &&
                g_strcmp0(gtk_widget_get_tooltip_text(widget), "Cancel at the next safe boundary") == 0;
        });
        require(cancel != nullptr, "Clipboard transfer must expose cancellation");
        g_signal_emit_by_name(cancel, "clicked");
        await([&] { return !gtk_widget_get_visible(gtk_widget_get_parent(progress)); });
        require(fs::exists(drop_source / "large.dat") &&
                !fs::exists(drop_destination / "large.dat"),
                "Cancelling a clipboard transfer must retain the source and avoid a partial public destination");
        close_and_drain(window);

        removal_batch(application, root / "batch-delete-confirmed", false, true, false);
        removal_batch(application, root / "batch-delete-cancelled", false, false, false);
        removal_batch(application, root / "batch-delete-late", false, true, true);
        removal_batch(application, root / "batch-trash", true, true, false);

        window = window_at(application, folder);
        auto *directory = FileManagerWindowTestAccess::directory(*window);
        DirectoryMutationGuard first(window->native_window(), directory);
        DirectoryMutationGuard second(window->native_window(), directory);
        require(first.begin() && second.begin(), "Live mutation leases must be admitted");
        require(!first.begin(), "A controller must not admit a second worker on one lease");
        require(!gtk_directory_list_get_monitored(directory), "Concurrent mutations must pause monitoring");
        require(first.finish() && !gtk_directory_list_get_monitored(directory),
                "One completion must not restart monitoring while another mutation is active");
        require(second.finish() && gtk_directory_list_get_monitored(directory),
                "The final completion must restart monitoring and enumerate results");
        await([&] { return !gtk_directory_list_is_loading(directory); });
        require(first.begin(), "A completed lease must be reusable");
        close_and_drain(window);
        require(!first.finish() && !gtk_directory_list_get_monitored(directory),
                "A destroyed window must never restart directory monitoring");
        require(!first.begin() && !second.begin(), "A destroyed window must refuse new mutations");

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

        const auto last_clipboard = root / "last-clipboard";
        auto *clipboard_application = gtk_application_new(
            "org.infiltrator.Files.LastClipboardTest", G_APPLICATION_NON_UNIQUE);
        g_signal_connect(clipboard_application, "activate", G_CALLBACK(+[](GtkApplication *app, gpointer data) {
            conflict_completion(app, *static_cast<const fs::path *>(data),
                                "Replace Conflicts", true, false, true);
        }), const_cast<fs::path *>(&last_clipboard));
        started_worker_flushes.store(0U);
        completed_worker_flushes.store(0U);
        delay_worker_flushes.store(true);
        const int clipboard_status = g_application_run(G_APPLICATION(clipboard_application), 0, nullptr);
        delay_worker_flushes.store(false);
        require(clipboard_status == 0 && completed_worker_flushes.load() >= 2U &&
                fs::exists(last_clipboard / "destination" / "other.txt") &&
                !fs::exists(last_clipboard / "source" / "other.txt"),
                "An admitted clipboard conflict transfer must finish before the last-window loop exits");
        g_object_unref(clipboard_application);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    g_object_unref(application);
    fs::remove_all(root);
    return 0;
}
