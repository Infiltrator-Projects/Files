// SPDX-License-Identifier: GPL-3.0-or-later
#include "recovery_operation_engine.hpp"
#include "destination_ownership.hpp"

#include <gio/gio.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <system_error>

namespace infiltrator::files {

namespace {

OperationResult result(const OperationStatus status,
                       const OperationPhase phase,
                       const std::filesystem::path &destination,
                       std::string message,
                       const bool changed = false,
                       std::vector<std::filesystem::path> retained_paths = {})
{
    return OperationResult{
        status, phase, destination, std::move(message), changed, std::move(retained_paths)};
}

std::string quoted_name(const std::filesystem::path &path)
{
    return "“" + path.filename().string() + "”";
}

bool path_present(const std::filesystem::path &path, std::error_code &error)
{
    error.clear();
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory ||
        error == std::errc::not_a_directory) {
        error.clear();
        return false;
    }
    if (error) {
        return false;
    }
    return status.type() != std::filesystem::file_type::not_found;
}

OperationStatus status_for_gerror(const GError *error) noexcept
{
    if (error == nullptr) {
        return OperationStatus::Success;
    }
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
        return OperationStatus::DestinationConflict;
    }
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
        return OperationStatus::UnsupportedCapability;
    }
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)) {
        return OperationStatus::PermissionFailure;
    }
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_READ_ONLY)) {
        return OperationStatus::ReadOnlyLocation;
    }
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) ||
        g_error_matches(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT) ||
        g_error_matches(error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME)) {
        return OperationStatus::InvalidRequest;
    }
    return OperationStatus::ExecutionFailure;
}

std::filesystem::path backup_path_for(const std::filesystem::path &destination,
                                      std::error_code &error)
{
    error.clear();
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string base = ".infiltrator-replace-" + destination.filename().string() + "-" +
                             std::to_string(tick);

    for (std::uint32_t attempt = 0U; attempt < 1024U; ++attempt) {
        const std::filesystem::path candidate =
            destination.parent_path() / (base + "-" + std::to_string(attempt));
        if (!path_present(candidate, error)) {
            if (!error) {
                return candidate;
            }
            return {};
        }
    }

    error = std::make_error_code(std::errc::file_exists);
    return {};
}

bool stage_destination(const std::filesystem::path &destination,
                       const std::filesystem::path &backup,
                       destination_ownership::OwnedOutput &staged,
                       std::error_code &error,
                       std::filesystem::path *retained_quarantine = nullptr)
{
    const destination_ownership::Identity identity =
        destination_ownership::identity_for(destination, error);
    if (error) {
        return false;
    }
    return destination_ownership::move_owned_no_replace(
        destination_ownership::OwnedOutput{destination, identity},
        backup,
        staged,
        error,
        retained_quarantine);
}

bool rollback_destination(const std::filesystem::path &destination,
                          const destination_ownership::OwnedOutput &staged,
                          std::string &detail,
                          std::filesystem::path &retained_path)
{
    retained_path.clear();
    std::error_code restore_error;
    std::filesystem::path retained_quarantine;
    if (!destination_ownership::restore_owned(
            staged, destination, restore_error, &retained_quarantine)) {
        const std::filesystem::path retained =
            retained_quarantine.empty() ? staged.path : retained_quarantine;
        retained_path = retained;
        detail = "The previous destination could not be restored safely from “" +
                 retained.string() + "”: " + restore_error.message();
        return false;
    }
    return true;
}

OperationResult cleanup_replaced_destination(const std::filesystem::path &destination,
                                             const destination_ownership::OwnedOutput &staged,
                                             std::string success_message)
{
    std::error_code cleanup_error;
    std::filesystem::path retained_quarantine;
    if (!destination_ownership::remove_owned_tree(
            staged, cleanup_error, &retained_quarantine)) {
        const std::filesystem::path retained =
            retained_quarantine.empty() ? staged.path : retained_quarantine;
        return result(OperationStatus::VerificationFailure,
                      OperationPhase::Verify,
                      destination,
                      std::move(success_message) +
                          " The previous destination was retained at “" + retained.string() +
                          "” because ownership-safe cleanup failed: " + cleanup_error.message(),
                      true,
                      {retained});
    }
    return result(OperationStatus::Success,
                  OperationPhase::Complete,
                  destination,
                  std::move(success_message),
                  true);
}

bool same_existing_object(const std::filesystem::path &left,
                          const std::filesystem::path &right)
{
    std::error_code error;
    const bool same = std::filesystem::equivalent(left, right, error);
    return !error && same;
}

std::string staging_failure_message(const std::error_code &error,
                                    const std::filesystem::path &retained)
{
    if (OperationEngine::status_for_error(error) ==
        OperationStatus::UnsupportedCapability) {
        return "This location does not provide the atomic no-replace operation required for safe replacement. The source and destination were left unchanged.";
    }
    return "The existing destination could not be staged safely: " + error.message() +
           (retained.empty() ? std::string{}
                             : " The original destination was retained at “" +
                                   retained.string() + "”.");
}

void append_unique_retained(
    std::vector<std::filesystem::path> &paths,
    const std::filesystem::path &path)
{
    if (!path.empty() &&
        std::find(paths.begin(), paths.end(), path) == paths.end()) {
        paths.push_back(path);
    }
}

void normalize_retained(std::vector<std::filesystem::path> &paths)
{
    std::vector<std::filesystem::path> normalized;
    normalized.reserve(paths.size());
    for (const auto &path : paths) {
        append_unique_retained(normalized, path);
    }
    paths = std::move(normalized);
}

std::string retained_paths_message(
    const std::vector<std::filesystem::path> &paths)
{
    std::string message;
    for (const auto &path : paths) {
        message += " Remaining data is available at “" + path.string() + "”.";
    }
    return message;
}

void append_missing_retained_guidance(
    std::string &message,
    const std::vector<std::filesystem::path> &paths)
{
    for (const auto &path : paths) {
        if (message.find(path.string()) == std::string::npos) {
            message += " Remaining data is available at “" + path.string() + "”.";
        }
    }
}

} // namespace

OperationResult RecoveryOperationEngine::replace_copy(
    const std::filesystem::path &source,
    const std::filesystem::path &destination_parent) const
{
    OperationEngine ordinary;
    const auto copy_item = [this, &ordinary](const std::filesystem::path &item,
                                              const std::filesystem::path &parent) {
        return hooks_.copy_item != nullptr ? hooks_.copy_item(item, parent)
                                           : ordinary.copy_item(item, parent);
    };
    OperationResult initial = copy_item(source, destination_parent);
    if (initial.status != OperationStatus::DestinationConflict) {
        return initial;
    }

    const std::filesystem::path destination = destination_parent / source.filename();
    if (same_existing_object(source, destination)) {
        return result(OperationStatus::InvalidRequest,
                      OperationPhase::Preflight,
                      destination,
                      "An item cannot replace itself.");
    }

    std::error_code error;
    const std::filesystem::path backup = backup_path_for(destination, error);
    if (error || backup.empty()) {
        return result(OperationEngine::status_for_error(error),
                      OperationPhase::Preflight,
                      destination,
                      "A safe replacement staging path could not be reserved.");
    }

    destination_ownership::OwnedOutput staged;
    std::filesystem::path staging_quarantine;
    if (!stage_destination(destination, backup, staged, error, &staging_quarantine)) {
        return result(OperationEngine::status_for_error(error),
                      OperationPhase::Execute,
                      destination,
                      staging_failure_message(error, staging_quarantine),
                      !staging_quarantine.empty(),
                      staging_quarantine.empty()
                          ? std::vector<std::filesystem::path>{}
                          : std::vector<std::filesystem::path>{staging_quarantine});
    }

    OperationResult replacement = copy_item(source, destination_parent);
    if (replacement.ok()) {
        return cleanup_replaced_destination(
            destination, staged, "Replaced " + quoted_name(destination) + ".");
    }

    std::string rollback_detail;
    std::filesystem::path rollback_retained;
    if (hooks_.before_rollback != nullptr) {
        hooks_.before_rollback(destination);
    }
    if (!rollback_destination(destination, staged, rollback_detail, rollback_retained)) {
        auto retained_paths = replacement.retained_paths;
        append_unique_retained(retained_paths, rollback_retained);
        normalize_retained(retained_paths);
        const std::string failure_message =
            "The replacement failed and Files could not restore the previous destination. " +
            rollback_detail + retained_paths_message(retained_paths);
        return result(OperationStatus::VerificationFailure,
                      OperationPhase::Verify,
                      destination,
                      failure_message,
                      true,
                      std::move(retained_paths));
    }

    replacement.destination = destination;
    normalize_retained(replacement.retained_paths);
    replacement.changed = !replacement.retained_paths.empty();
    if (!replacement.message.empty()) {
        replacement.message += " The previous destination was restored.";
    } else {
        replacement.message = "The replacement failed. The previous destination was restored.";
    }
    append_missing_retained_guidance(replacement.message, replacement.retained_paths);
    return replacement;
}

OperationResult RecoveryOperationEngine::replace_move(
    const std::filesystem::path &source,
    const std::filesystem::path &destination_parent) const
{
    OperationEngine ordinary;
    const auto move_item = [this, &ordinary](const std::filesystem::path &item,
                                              const std::filesystem::path &parent) {
        return hooks_.move_item != nullptr ? hooks_.move_item(item, parent)
                                           : ordinary.move_item(item, parent);
    };
    OperationResult initial = move_item(source, destination_parent);
    if (initial.status != OperationStatus::DestinationConflict) {
        return initial;
    }

    const std::filesystem::path destination = destination_parent / source.filename();
    if (same_existing_object(source, destination)) {
        return result(OperationStatus::InvalidRequest,
                      OperationPhase::Preflight,
                      destination,
                      "An item cannot replace itself.");
    }

    std::error_code error;
    const std::filesystem::path backup = backup_path_for(destination, error);
    if (error || backup.empty()) {
        return result(OperationEngine::status_for_error(error),
                      OperationPhase::Preflight,
                      destination,
                      "A safe replacement staging path could not be reserved.");
    }

    destination_ownership::OwnedOutput staged;
    std::filesystem::path staging_quarantine;
    if (!stage_destination(destination, backup, staged, error, &staging_quarantine)) {
        return result(OperationEngine::status_for_error(error),
                      OperationPhase::Execute,
                      destination,
                      staging_failure_message(error, staging_quarantine),
                      !staging_quarantine.empty(),
                      staging_quarantine.empty()
                          ? std::vector<std::filesystem::path>{}
                          : std::vector<std::filesystem::path>{staging_quarantine});
    }

    OperationResult replacement = move_item(source, destination_parent);
    if (replacement.ok()) {
        return cleanup_replaced_destination(
            destination, staged, "Replaced " + quoted_name(destination) + " by moving the selected item.");
    }

    std::error_code source_error;
    const bool source_still_exists = path_present(source, source_error);
    if (!source_error && source_still_exists) {
        std::string rollback_detail;
        std::filesystem::path rollback_retained;
        if (hooks_.before_rollback != nullptr) {
            hooks_.before_rollback(destination);
        }
        if (rollback_destination(destination,
                                 staged,
                                 rollback_detail,
                                 rollback_retained)) {
            replacement.destination = destination;
            normalize_retained(replacement.retained_paths);
            replacement.changed = !replacement.retained_paths.empty();
            if (!replacement.message.empty()) {
                replacement.message += " The previous destination was restored.";
            } else {
                replacement.message = "The replacement move failed. The previous destination was restored.";
            }
            append_missing_retained_guidance(
                replacement.message, replacement.retained_paths);
            return replacement;
        }
        auto retained_paths = replacement.retained_paths;
        append_unique_retained(retained_paths, rollback_retained);
        normalize_retained(retained_paths);
        const std::string failure_message =
            "The replacement move failed and Files could not restore the previous destination. " +
            rollback_detail + retained_paths_message(retained_paths);
        return result(OperationStatus::VerificationFailure,
                      OperationPhase::Verify,
                      destination,
                      failure_message,
                      true,
                      std::move(retained_paths));
    }

    auto retained_paths = replacement.retained_paths;
    append_unique_retained(retained_paths, backup);
    normalize_retained(retained_paths);
    const std::string failure_message =
        "The replacement move did not verify cleanly. Files retained all remaining data rather than risking data loss." +
        retained_paths_message(retained_paths);
    return result(OperationStatus::VerificationFailure,
                  OperationPhase::Verify,
                  destination,
                  failure_message,
                  true,
                  std::move(retained_paths));
}

OperationResult RecoveryOperationEngine::trash_item(const std::filesystem::path &source) const
{
    if (source.empty() || source.filename().empty()) {
        return result(OperationStatus::InvalidRequest,
                      OperationPhase::Preflight,
                      {},
                      "The selected item cannot be moved to Trash.");
    }

    std::error_code error;
    const bool source_exists = path_present(source, error);
    if (error || !source_exists) {
        return result(error ? OperationEngine::status_for_error(error)
                            : OperationStatus::InvalidRequest,
                      OperationPhase::Preflight,
                      {},
                      "The selected item no longer exists.");
    }

    GFile *file = g_file_new_for_path(source.string().c_str());
    GError *gio_error = nullptr;
    const gboolean trashed = g_file_trash(file, nullptr, &gio_error);
    g_object_unref(file);

    if (!trashed) {
        const OperationStatus status = status_for_gerror(gio_error);
        const std::string detail = gio_error != nullptr ? gio_error->message : "Unknown error";
        if (gio_error != nullptr) {
            g_error_free(gio_error);
        }
        return result(status,
                      OperationPhase::Execute,
                      {},
                      "The item could not be moved to Trash: " + detail);
    }

    const bool source_still_exists = path_present(source, error);
    if (error || source_still_exists) {
        return result(OperationStatus::VerificationFailure,
                      OperationPhase::Verify,
                      {},
                      "The Trash operation completed but the original path could not be verified as removed.",
                      true);
    }

    return result(OperationStatus::Success,
                  OperationPhase::Complete,
                  {},
                  "Moved " + quoted_name(source) + " to Trash.",
                  true);
}

OperationResult RecoveryOperationEngine::restore_item(
    const std::string_view trash_uri,
    const std::filesystem::path &original_path,
    const bool replace_existing) const
{
    if (trash_uri.empty() || original_path.empty() || original_path.filename().empty()) {
        return result(OperationStatus::InvalidRequest,
                      OperationPhase::Preflight,
                      original_path,
                      "The Trash item does not contain a usable original location.");
    }

    std::error_code error;
    const std::filesystem::file_status parent_status =
        std::filesystem::status(original_path.parent_path(), error);
    if (error || !std::filesystem::is_directory(parent_status)) {
        return result(error ? OperationEngine::status_for_error(error)
                            : OperationStatus::InvalidRequest,
                      OperationPhase::Preflight,
                      original_path,
                      "The original folder is no longer available.");
    }

    GFile *trash_file = g_file_new_for_uri(std::string(trash_uri).c_str());
    GError *query_error = nullptr;
    GFileInfo *trash_info = g_file_query_info(trash_file,
                                              G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                              G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                              nullptr,
                                              &query_error);
    if (trash_info == nullptr) {
        const OperationStatus status = status_for_gerror(query_error);
        const std::string detail = query_error != nullptr ? query_error->message : "Unknown error";
        if (query_error != nullptr) {
            g_error_free(query_error);
        }
        g_object_unref(trash_file);
        return result(status,
                      OperationPhase::Preflight,
                      original_path,
                      "The Trash item is no longer available: " + detail);
    }
    g_object_unref(trash_info);

    const bool destination_exists = path_present(original_path, error);
    if (error) {
        g_object_unref(trash_file);
        return result(OperationEngine::status_for_error(error),
                      OperationPhase::Preflight,
                      original_path,
                      "The original destination could not be inspected.");
    }
    if (destination_exists && !replace_existing) {
        g_object_unref(trash_file);
        return result(OperationStatus::DestinationConflict,
                      OperationPhase::Preflight,
                      original_path,
                      "An item named " + quoted_name(original_path) +
                          " already exists in the original folder.");
    }

    std::filesystem::path backup;
    destination_ownership::OwnedOutput staged;
    if (destination_exists) {
        backup = backup_path_for(original_path, error);
        if (error || backup.empty()) {
            g_object_unref(trash_file);
            return result(OperationEngine::status_for_error(error),
                          OperationPhase::Preflight,
                          original_path,
                          "A safe restore replacement staging path could not be reserved.");
        }
        std::filesystem::path staging_quarantine;
        if (!stage_destination(
                original_path, backup, staged, error, &staging_quarantine)) {
            g_object_unref(trash_file);
            return result(OperationEngine::status_for_error(error),
                          OperationPhase::Execute,
                          original_path,
                          staging_failure_message(error, staging_quarantine),
                          !staging_quarantine.empty(),
                          staging_quarantine.empty()
                              ? std::vector<std::filesystem::path>{}
                              : std::vector<std::filesystem::path>{staging_quarantine});
        }
    }

    GFile *destination_file = g_file_new_for_path(original_path.string().c_str());
    GError *move_error = nullptr;
    const gboolean moved = g_file_move(trash_file,
                                       destination_file,
                                       G_FILE_COPY_NOFOLLOW_SYMLINKS,
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       &move_error);
    g_object_unref(destination_file);

    if (!moved) {
        const OperationStatus status = status_for_gerror(move_error);
        const std::string detail = move_error != nullptr ? move_error->message : "Unknown error";
        if (move_error != nullptr) {
            g_error_free(move_error);
        }

        if (!backup.empty()) {
            const bool destination_now_exists = path_present(original_path, error);
            const bool trash_still_exists = g_file_query_exists(trash_file, nullptr) != FALSE;
            if (!error && !destination_now_exists && trash_still_exists) {
                std::string rollback_detail;
                std::filesystem::path rollback_retained;
                if (!rollback_destination(original_path,
                                          staged,
                                          rollback_detail,
                                          rollback_retained)) {
                    g_object_unref(trash_file);
                    return result(OperationStatus::VerificationFailure,
                                  OperationPhase::Verify,
                                  original_path,
                                  "Restore failed and the previous destination could not be restored. " +
                                      rollback_detail,
                                  true,
                                  {rollback_retained});
                }
            } else {
                g_object_unref(trash_file);
                return result(OperationStatus::VerificationFailure,
                              OperationPhase::Verify,
                              original_path,
                              "Restore failed in an uncertain state. Files retained the staged previous destination at “" +
                                  backup.string() + "” rather than risking data loss.",
                              true,
                              {backup});
            }
        }

        g_object_unref(trash_file);
        return result(status,
                      OperationPhase::Execute,
                      original_path,
                      "The item could not be restored: " + detail);
    }

    g_object_unref(trash_file);

    const bool restored = path_present(original_path, error);
    if (error || !restored) {
        return result(OperationStatus::VerificationFailure,
                      OperationPhase::Verify,
                      original_path,
                      "The restore completed but the restored item could not be verified.",
                      true);
    }

    if (!backup.empty()) {
        return cleanup_replaced_destination(
            original_path, staged, "Restored and replaced " + quoted_name(original_path) + ".");
    }

    return result(OperationStatus::Success,
                  OperationPhase::Complete,
                  original_path,
                  "Restored " + quoted_name(original_path) + ".",
                  true);
}

OperationResult RecoveryOperationEngine::delete_item(const std::filesystem::path &source) const
{
    if (source.empty() || source.filename().empty()) {
        return result(OperationStatus::InvalidRequest,
                      OperationPhase::Preflight,
                      {},
                      "The selected item cannot be permanently deleted.");
    }

    std::error_code error;
    const bool source_exists = path_present(source, error);
    if (error || !source_exists) {
        return result(error ? OperationEngine::status_for_error(error)
                            : OperationStatus::InvalidRequest,
                      OperationPhase::Preflight,
                      {},
                      "The selected item no longer exists.");
    }

    (void)std::filesystem::remove_all(source, error);
    if (error) {
        return result(OperationEngine::status_for_error(error),
                      OperationPhase::Execute,
                      {},
                      "The item could not be permanently deleted: " + error.message());
    }

    const bool still_exists = path_present(source, error);
    if (error || still_exists) {
        return result(OperationStatus::VerificationFailure,
                      OperationPhase::Verify,
                      {},
                      "Deletion completed but removal could not be verified.",
                      true);
    }

    return result(OperationStatus::Success,
                  OperationPhase::Complete,
                  {},
                  "Permanently deleted " + quoted_name(source) + ".",
                  true);
}

OperationResult RecoveryOperationEngine::delete_uri(const std::string_view uri) const
{
    if (uri.empty()) {
        return result(OperationStatus::InvalidRequest,
                      OperationPhase::Preflight,
                      {},
                      "The selected Trash item is not available.");
    }

    GFile *file = g_file_new_for_uri(std::string(uri).c_str());
    GError *delete_error = nullptr;
    const gboolean deleted = g_file_delete(file, nullptr, &delete_error);
    if (!deleted) {
        const OperationStatus status = status_for_gerror(delete_error);
        const std::string detail = delete_error != nullptr ? delete_error->message : "Unknown error";
        if (delete_error != nullptr) {
            g_error_free(delete_error);
        }
        g_object_unref(file);
        return result(status,
                      OperationPhase::Execute,
                      {},
                      "The Trash item could not be permanently deleted: " + detail);
    }

    const bool still_exists = g_file_query_exists(file, nullptr) != FALSE;
    g_object_unref(file);
    if (still_exists) {
        return result(OperationStatus::VerificationFailure,
                      OperationPhase::Verify,
                      {},
                      "Deletion completed but the Trash item still appears to exist.",
                      true);
    }

    return result(OperationStatus::Success,
                  OperationPhase::Complete,
                  {},
                  "Permanently deleted the Trash item.",
                  true);
}

} // namespace infiltrator::files
