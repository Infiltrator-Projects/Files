// SPDX-License-Identifier: GPL-3.0-or-later
#include "provider_import_engine.hpp"
#include "transfer_operation_engine.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <unistd.h>

namespace infiltrator::files {
namespace {
namespace fs = std::filesystem;
constexpr std::uint64_t kMaxImport = 1024ULL * 1024ULL * 1024ULL;
template <typename T> struct Unref {
    void operator()(T *ptr) const noexcept { if (ptr) g_object_unref(ptr); }
};
template <typename T> using GOwned = std::unique_ptr<T, Unref<T>>;

OperationResult failure(OperationStatus status, OperationPhase phase,
                        const fs::path &target, const std::string &message)
{
    return {status, phase, target, message, false, {}};
}
std::string message_from(GError *error, const char *fallback)
{
    return error != nullptr ? error->message : fallback;
}
std::string source_error(GError *error, GFile *source, const char *fallback)
{
    const std::string why = message_from(error, fallback);
    const bool portable = g_file_has_uri_scheme(source, "mtp") ||
                          g_file_has_uri_scheme(source, "gphoto2");
    const bool disconnected = error != nullptr &&
        (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_MOUNTED) ||
         g_error_matches(error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE) ||
         g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NETWORK_UNREACHABLE) ||
         g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED) ||
         g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT));
    if (disconnected)
        return (portable ? "Portable device disconnected: " :
                           "Remote provider disconnected: ") + why;
    if (portable && error != nullptr &&
        g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        return "Portable device item disappeared: " + why;
    return why;
}
struct ProgressBudget { GCancellable *cancellable; bool exceeded{false}; };
void import_progress(goffset copied, goffset total, gpointer user_data)
{
    auto *budget = static_cast<ProgressBudget *>(user_data);
    if ((copied >= 0 && static_cast<std::uint64_t>(copied) > kMaxImport) ||
        (total >= 0 && static_cast<std::uint64_t>(total) > kMaxImport)) {
        budget->exceeded = true;
        g_cancellable_cancel(budget->cancellable);
    }
}
bool cancelled(GCancellable *cancellable)
{
    return cancellable != nullptr && g_cancellable_is_cancelled(cancellable);
}
bool verify_against_provider(GFile *original, GFile *staged,
                             GCancellable *cancel, std::string &reason)
{
    GError *error = nullptr;
    GOwned<GFileInputStream> remote(g_file_read(original, cancel, &error));
    if (!remote) {
        reason = "Could not reopen the provider object to verify imported bytes: " +
                 source_error(error, original, "unknown provider error");
        g_clear_error(&error);
        return false;
    }
    GOwned<GFileInputStream> local(g_file_read(staged, cancel, &error));
    if (!local) {
        reason = "Could not read the private import for verification: " +
                 message_from(error, "unknown local error");
        g_clear_error(&error);
        return false;
    }
    std::array<char, 65536> a{}, b{};
    while (true) {
        gsize left = 0, right = 0;
        gboolean first = g_input_stream_read_all(G_INPUT_STREAM(remote.get()),
                                                  a.data(), a.size(), &left, cancel, &error);
        if (!first) {
            reason = "Provider reread failed: " + source_error(error, original, "unknown read error");
            g_clear_error(&error);
            return false;
        }
        gboolean second = g_input_stream_read_all(G_INPUT_STREAM(local.get()),
                                                   b.data(), b.size(), &right, cancel, &error);
        if (!second) {
            reason = "Private copy reread failed: " + message_from(error, "unknown read error");
            g_clear_error(&error);
            return false;
        }
        if (left != right || std::memcmp(a.data(), b.data(), left) != 0) {
            reason = "The source provider and private import returned different bytes; no destination was published.";
            return false;
        }
        if (left == 0) return true;
    }
}
bool safe_journal_id(const std::string &id)
{
    if (id.empty() || id.size() > 100) return false;
    for (const char c : id)
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
            !(c >= '0' && c <= '9') && c != '-') return false;
    return true;
}
} // namespace

OperationResult ProviderImportEngine::import_regular_file(
    const std::string &uri, const fs::path &destination_parent,
    const std::string &journal_id, GCancellable *cancel) const
{
    if (uri.empty() || !destination_parent.is_absolute() ||
        !safe_journal_id(journal_id)) {
        return failure(OperationStatus::InvalidRequest, OperationPhase::Preflight,
                       destination_parent, "Import requires a provider URI, native destination and durable operation ID.");
    }
    if (cancelled(cancel))
        return failure(OperationStatus::Cancelled, OperationPhase::Preflight,
                       destination_parent, "Provider import cancelled before starting.");
    GOwned<GFile> source(g_file_new_for_uri(uri.c_str()));
    GError *error = nullptr;
    GOwned<GFileInfo> metadata(g_file_query_info(
        source.get(), G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, &error));
    if (!metadata) {
        const std::string why = source_error(error, source.get(), "provider unavailable");
        const bool was_cancelled = cancelled(cancel);
        g_clear_error(&error);
        return failure(was_cancelled ? OperationStatus::Cancelled : OperationStatus::ExecutionFailure,
                       OperationPhase::Preflight, destination_parent,
                       "Cannot inspect the provider source: " + why);
    }
    if (g_file_info_get_file_type(metadata.get()) != G_FILE_TYPE_REGULAR)
        return failure(OperationStatus::UnsupportedCapability, OperationPhase::Preflight,
                       destination_parent, "Only regular provider files may be imported. Directories and links are not imported.");
    if (g_file_info_has_attribute(metadata.get(), G_FILE_ATTRIBUTE_STANDARD_SIZE) &&
        g_file_info_get_size(metadata.get()) > static_cast<goffset>(kMaxImport))
        return failure(OperationStatus::UnsupportedCapability, OperationPhase::Preflight,
                       destination_parent, "Provider file exceeds the qualified 1 GiB import limit.");
    const char *raw = g_file_info_get_name(metadata.get());
    if (raw == nullptr || !OperationEngine::validate_item_name(raw))
        return failure(OperationStatus::InvalidRequest, OperationPhase::Preflight,
                       destination_parent, "The provider name cannot be safely represented as a native filename.");

    std::error_code ec;
    if (!fs::is_directory(destination_parent, ec) || ec)
        return failure(OperationStatus::InvalidRequest, OperationPhase::Preflight,
                       destination_parent, "The native import destination is not an accessible directory.");
    const fs::path final = destination_parent / raw;
    // A same-volume private directory is intentionally named with the journal
    // identity, so interruption evidence can identify an orphaned staging tree.
    std::string pattern = (destination_parent /
        (".files-provider-import-" + journal_id + "-XXXXXX")).string();
    if (::mkdtemp(pattern.data()) == nullptr)
        return failure(OperationStatus::PermissionFailure, OperationPhase::Preflight,
                       final, "Could not create a private staging directory: " +
                              std::error_code(errno, std::generic_category()).message());
    const fs::path staging_dir(pattern);
    const fs::path staging_file = staging_dir / raw;

    OperationResult result;
    if (cancelled(cancel)) {
        result = failure(OperationStatus::Cancelled, OperationPhase::Execute,
                         final, "Provider import cancelled.");
    } else {
        GOwned<GFile> stage(g_file_new_for_path(staging_file.c_str()));
        // Progress remains authoritative when a device supplies no trustworthy size.
        GOwned<GCancellable> local_cancel(g_cancellable_new());
        GCancellable *effective = cancel != nullptr ? cancel : local_cancel.get();
        ProgressBudget budget{effective};
        if (!g_file_copy(source.get(), stage.get(), G_FILE_COPY_NOFOLLOW_SYMLINKS,
                         effective, import_progress, &budget, &error)) {
            const bool was_cancelled = cancelled(cancel);
            result = failure(budget.exceeded ? OperationStatus::UnsupportedCapability :
                             was_cancelled ? OperationStatus::Cancelled :
                                             OperationStatus::ExecutionFailure,
                             OperationPhase::Execute, final,
                             budget.exceeded ? "Provider data exceeded the 1 GiB import limit." :
                             "Provider import failed: " + source_error(error, source.get(), "unknown I/O error"));
            g_clear_error(&error);
        } else {
            std::string verification;
            if (!verify_against_provider(source.get(), stage.get(), cancel, verification)) {
                result = failure(cancelled(cancel) ? OperationStatus::Cancelled
                                                  : OperationStatus::VerificationFailure,
                                 OperationPhase::Verify, final, verification);
            } else if (cancelled(cancel)) {
                result = failure(OperationStatus::Cancelled, OperationPhase::Verify,
                                 final, "Provider import cancelled before publication.");
            } else {
                // The existing engine owns conflict detection, atomic publication,
                // metadata verification and durable directory barriers.
                TransferOperationEngine engine;
                result = engine.copy_item(staging_file, destination_parent,
                                          ConflictPolicy::Fail);
            }
        }
    }
    std::error_code cleanup;
    fs::remove_all(staging_dir, cleanup);
    if (cleanup) {
        result.retained_paths.push_back(staging_dir);
        result.changed = true;
        result.message += " Private provider staging could not be removed: " + cleanup.message();
        if (result.ok()) {
            result.status = OperationStatus::VerificationFailure;
            result.phase = OperationPhase::Verify;
        }
    }
    return result;
}

} // namespace infiltrator::files
