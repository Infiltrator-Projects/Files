// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_engine.hpp"
#include "destination_ownership.hpp"
#include "transfer_operation_engine.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace infiltrator::files {

namespace {

struct CreateDirectoryPlan {
    std::filesystem::path parent;
    std::filesystem::path destination;
};

OperationResult failure(const OperationStatus status,
                        const OperationPhase phase,
                        const std::filesystem::path &destination,
                        std::string message,
                        const bool changed = false,
                        std::vector<std::filesystem::path> retained_paths = {})
{
    return OperationResult{
        status, phase, destination, std::move(message), changed, std::move(retained_paths)};
}

std::string retained_paths_message(
    const std::vector<std::filesystem::path> &retained_paths)
{
    std::string message;
    for (const auto &path : retained_paths) {
        message += " Remaining data is available at “" + path.string() + "”.";
    }
    return message;
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

} // namespace

bool OperationEngine::validate_item_name(const std::string_view name, std::string *reason)
{
    auto reject = [reason](const char *message) {
        if (reason != nullptr) {
            *reason = message;
        }
        return false;
    };

    if (name.empty()) {
        return reject("Name cannot be empty.");
    }
    if (name == "." || name == "..") {
        return reject("That name is reserved.");
    }
    if (name.find('/') != std::string_view::npos) {
        return reject("Names cannot contain '/'.");
    }
    if (name.find('\0') != std::string_view::npos) {
        return reject("Names cannot contain a null character.");
    }

    if (reason != nullptr) {
        reason->clear();
    }
    return true;
}

bool OperationEngine::validate_directory_name(const std::string_view name, std::string *reason)
{
    return validate_item_name(name, reason);
}

OperationStatus OperationEngine::status_for_error(const std::error_code &error) noexcept
{
    if (!error) {
        return OperationStatus::Success;
    }
    if (error == std::errc::operation_not_supported ||
        error == std::errc::function_not_supported) {
        return OperationStatus::UnsupportedCapability;
    }
    if (error == std::errc::file_exists) {
        return OperationStatus::DestinationConflict;
    }
    if (error == std::errc::permission_denied || error == std::errc::operation_not_permitted) {
        return OperationStatus::PermissionFailure;
    }
    if (error == std::errc::read_only_file_system) {
        return OperationStatus::ReadOnlyLocation;
    }
    if (error == std::errc::filename_too_long || error == std::errc::invalid_argument ||
        error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory) {
        return OperationStatus::InvalidRequest;
    }
    return OperationStatus::ExecutionFailure;
}

OperationResult OperationEngine::create_directory(const std::filesystem::path &parent,
                                                   const std::string_view name) const
{
    std::string validation_error;
    if (!validate_directory_name(name, &validation_error)) {
        return failure(OperationStatus::InvalidRequest,
                       OperationPhase::Preflight,
                       {},
                       std::move(validation_error));
    }
    if (parent.empty()) {
        return failure(OperationStatus::InvalidRequest,
                       OperationPhase::Preflight,
                       {},
                       "The destination folder is not available.");
    }

    const CreateDirectoryPlan plan{
        parent,
        parent / std::filesystem::path(std::string(name)),
    };

    std::error_code error;
    const std::filesystem::file_status parent_status = std::filesystem::status(plan.parent, error);
    if (error) {
        const OperationStatus status = status_for_error(error);
        return failure(status,
                       OperationPhase::Preflight,
                       plan.destination,
                       status == OperationStatus::PermissionFailure
                           ? "You do not have permission to create a folder here."
                           : "The destination folder is not available.");
    }
    if (!std::filesystem::is_directory(parent_status)) {
        return failure(OperationStatus::InvalidRequest,
                       OperationPhase::Preflight,
                       plan.destination,
                       "The destination is not a folder.");
    }

    // Avoid creating private staging for a conflict that is already visible.
    // This observation is deliberately not the namespace authority: another
    // writer may still create the destination immediately afterwards, so the
    // final publication below remains an atomic no-replace operation.
    const bool destination_exists = path_present(plan.destination, error);
    if (error) {
        return failure(status_for_error(error),
                       OperationPhase::Preflight,
                       plan.destination,
                       "The destination could not be checked.");
    }
    if (destination_exists) {
        return failure(OperationStatus::DestinationConflict,
                       OperationPhase::Preflight,
                       plan.destination,
                       "An item named " + quoted_name(plan.destination) + " already exists.");
    }

    // New Folder uses the same ownership boundary as copy/move: assemble the
    // directory under a descriptor-rooted private staging directory, publish
    // it with an atomic no-replace rename, and verify the recorded identity.
    destination_ownership::ProtectedStaging staging;
    destination_ownership::OwnedOutputs owned;
    if (!destination_ownership::create_protected_staging(plan.parent, staging, error)) {
        const OperationStatus status = status_for_error(error);
        const std::string message =
            status == OperationStatus::PermissionFailure
                ? "You do not have permission to create a folder here."
                : status == OperationStatus::ReadOnlyLocation
                      ? "This location is read-only."
                      : "A private folder staging area could not be created: " + error.message();
        return failure(status, OperationPhase::Execute, plan.destination, message);
    }

    if (::mkdirat(staging.descriptor, "payload", 0777) != 0) {
        error = std::error_code(errno, std::generic_category());
        return failure(status_for_error(error),
                       OperationPhase::Execute,
                       plan.destination,
                       "The folder could not be created: " + error.message());
    }

    const std::filesystem::path private_path = staging.access_root() / "payload";
    if (!destination_ownership::record_at(
            staging.descriptor, "payload", private_path, owned, error)) {
        std::error_code cleanup_error;
        std::vector<std::filesystem::path> retained_paths;
        const bool cleaned = destination_ownership::cleanup_unpublished(
            staging, plan.destination, owned, cleanup_error, &retained_paths);
        return failure(OperationStatus::VerificationFailure,
                       OperationPhase::Verify,
                       plan.destination,
                       "The new folder identity could not be recorded before publication." +
                           (cleaned ? std::string{} : retained_paths_message(retained_paths)),
                       !cleaned,
                       std::move(retained_paths));
    }

    if (!destination_ownership::publish_private_tree(
            staging, private_path, plan.destination, owned, error)) {
        const OperationStatus status = status_for_error(error);
        std::error_code cleanup_error;
        std::vector<std::filesystem::path> retained_paths;
        const bool cleaned = destination_ownership::cleanup_unpublished(
            staging, plan.destination, owned, cleanup_error, &retained_paths);

        std::string message;
        if (status == OperationStatus::DestinationConflict) {
            message = "An item named " + quoted_name(plan.destination) + " already exists.";
        } else if (status == OperationStatus::PermissionFailure) {
            message = "You do not have permission to create " + quoted_name(plan.destination) + " here.";
        } else if (status == OperationStatus::ReadOnlyLocation) {
            message = "This location is read-only.";
        } else if (status == OperationStatus::UnsupportedCapability) {
            message = "This location does not provide the atomic no-replace operation required for safe folder creation.";
        } else {
            message = "The folder could not be published: " + error.message();
        }
        if (!cleaned) {
            message += retained_paths_message(retained_paths);
        }
        return failure(status,
                       OperationPhase::Execute,
                       plan.destination,
                       std::move(message),
                       !cleaned,
                       std::move(retained_paths));
    }

    if (!destination_ownership::verify_all(owned, error)) {
        return failure(OperationStatus::VerificationFailure,
                       OperationPhase::Verify,
                       plan.destination,
                       "The folder was created but its published identity could not be verified.",
                       true);
    }

    return OperationResult{OperationStatus::Success,
                           OperationPhase::Complete,
                           plan.destination,
                           "Created " + quoted_name(plan.destination) + ".",
                           true,
                           {}};
}

OperationResult OperationEngine::rename_item(const std::filesystem::path &source,
                                              const std::string_view new_name) const
{
    std::string validation_error;
    if (!validate_item_name(new_name, &validation_error)) {
        return failure(OperationStatus::InvalidRequest,
                       OperationPhase::Preflight,
                       {},
                       std::move(validation_error));
    }
    if (source.empty() || source.filename().empty()) {
        return failure(OperationStatus::InvalidRequest,
                       OperationPhase::Preflight,
                       {},
                       "The selected item is not available.");
    }

    const std::filesystem::path destination =
        source.parent_path() / std::filesystem::path(std::string(new_name));
    if (destination == source) {
        std::error_code error;
        const bool source_exists = path_present(source, error);
        if (error || !source_exists) {
            return failure(error ? status_for_error(error) : OperationStatus::InvalidRequest,
                           OperationPhase::Preflight,
                           destination,
                           "The selected item no longer exists.");
        }
        return OperationResult{OperationStatus::Success,
                               OperationPhase::Complete,
                               destination,
                               "The name is unchanged.",
                               false,
                               {}};
    }

    std::error_code error;
    const bool source_exists = path_present(source, error);
    if (error || !source_exists) {
        const OperationStatus status = error ? status_for_error(error)
                                             : OperationStatus::InvalidRequest;
        return failure(status,
                       OperationPhase::Preflight,
                       destination,
                       status == OperationStatus::PermissionFailure
                           ? "You do not have permission to inspect the selected item."
                           : "The selected item no longer exists.");
    }

    const bool destination_exists = path_present(destination, error);
    if (error) {
        const OperationStatus status = status_for_error(error);
        return failure(status,
                       OperationPhase::Preflight,
                       destination,
                       "The destination could not be checked.");
    }
    if (destination_exists) {
        return failure(OperationStatus::DestinationConflict,
                       OperationPhase::Preflight,
                       destination,
                       "An item named " + quoted_name(destination) + " already exists.");
    }

    const auto moved_identity = destination_ownership::identity_for(source, error);
    if (error) {
        return failure(status_for_error(error),
                       OperationPhase::Preflight,
                       destination,
                       "The selected item identity could not be inspected.");
    }

    const destination_ownership::OwnedOutput source_output{source, moved_identity};
    destination_ownership::OwnedOutput moved_output;
    std::filesystem::path retained_quarantine;
    if (!destination_ownership::move_owned_no_replace(
            source_output, destination, moved_output, error, &retained_quarantine)) {
        const OperationStatus status = status_for_error(error);
        std::string message;
        if (status == OperationStatus::DestinationConflict) {
            message = "An item named " + quoted_name(destination) + " already exists.";
        } else if (status == OperationStatus::PermissionFailure) {
            message = "You do not have permission to rename " + quoted_name(source) + ".";
        } else if (status == OperationStatus::ReadOnlyLocation) {
            message = "This location is read-only.";
        } else if (status == OperationStatus::UnsupportedCapability) {
            message = "This location does not provide the atomic no-replace operation required for a safe rename. The item was left unchanged.";
        } else {
            message = "The item could not be renamed: " + error.message();
        }
        if (!retained_quarantine.empty()) {
            message += " The original item was retained safely at “" +
                       retained_quarantine.string() + "”.";
        }
        std::vector<std::filesystem::path> retained_paths;
        if (!retained_quarantine.empty()) {
            retained_paths.push_back(retained_quarantine);
        }
        return failure(status,
                       OperationPhase::Execute,
                       destination,
                       std::move(message),
                       !retained_quarantine.empty(),
                       std::move(retained_paths));
    }

    const bool destination_is_source =
        destination_ownership::same_object(destination, moved_output.identity, error);
    if (error || !destination_is_source) {
        return failure(OperationStatus::VerificationFailure,
                       OperationPhase::Verify,
                       destination,
                       "The rename completed but the new item could not be verified.",
                       true);
    }
    const bool source_still_exists = path_present(source, error);
    if (error || source_still_exists) {
        return failure(OperationStatus::VerificationFailure,
                       OperationPhase::Verify,
                       destination,
                       "The rename completed but the old path still appears to exist.",
                       true);
    }

    return OperationResult{OperationStatus::Success,
                           OperationPhase::Complete,
                           destination,
                           "Renamed to " + quoted_name(destination) + ".",
                           true,
                           {}};
}

OperationResult OperationEngine::copy_item(const std::filesystem::path &source,
                                            const std::filesystem::path &destination_parent) const
{
    return TransferOperationEngine{}.copy_item(
        source, destination_parent, ConflictPolicy::Fail, nullptr);
}

OperationResult OperationEngine::move_item(const std::filesystem::path &source,
                                            const std::filesystem::path &destination_parent) const
{
    return TransferOperationEngine{}.move_item(
        source, destination_parent, ConflictPolicy::Fail, nullptr);
}

} // namespace infiltrator::files
