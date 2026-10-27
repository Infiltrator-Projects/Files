// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_engine.hpp"
#include "destination_ownership.hpp"
#include "transfer_operation_engine.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>
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



OperationResult OperationEngine::change_permissions(
    const std::filesystem::path &source, const unsigned int permissions) const
{
    if (!source.is_absolute() || source.empty() || permissions > 0777U) {
        return failure(OperationStatus::InvalidRequest, OperationPhase::Preflight,
                       source, "Permissions require an absolute native path and a mode from 0000 to 0777.");
    }

    // A held descriptor, rather than a repeated pathname lookup, owns the inode.
    // O_RDONLY cannot open a legitimate mode-000 file even for its owner. Use
    // O_PATH only on EACCES and never follow the selected symlink.
    int fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    bool path_only = false;
    if (fd < 0 && errno == EACCES) {
        fd = ::open(source.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW);
        path_only = fd >= 0;
    }
    if (fd < 0) {
        const std::error_code error(errno, std::generic_category());
        return failure(status_for_error(error), OperationPhase::Preflight, source,
                       "Could not open the selected item without following symlinks: " +
                           error.message());
    }
    const auto close_fd = [fd]() { return ::close(fd); };
    struct stat before {};
    if (::fstat(fd, &before) != 0) {
        const std::error_code error(errno, std::generic_category());
        (void)close_fd();
        return failure(status_for_error(error), OperationPhase::Preflight, source,
                       "Could not identify the selected item: " + error.message());
    }
    if (!S_ISREG(before.st_mode) && !S_ISDIR(before.st_mode)) {
        (void)close_fd();
        return failure(OperationStatus::UnsupportedCapability, OperationPhase::Preflight,
                       source, "Permission editing is supported only for native regular files and directories.");
    }
    if ((before.st_mode & 07000) != 0) {
        (void)close_fd();
        return failure(OperationStatus::UnsupportedCapability, OperationPhase::Preflight,
                       source, "Special setuid/setgid/sticky bits require an explicit specialised editor.");
    }
    // chmod may rewrite the effective POSIX ACL mask on ACL-bearing files.
    // Refuse rather than silently modifying access rights beyond the user's
    // three ordinary mode fields. Unknown ACL readability also fails closed.
    // Linux O_PATH descriptors cannot be passed to fgetxattr. The /proc fd
    // reference binds the ACL query to precisely the inode held above; it
    // must not fall back to the mutable original pathname on failure.
    const std::string proc_reference = "/proc/self/fd/" + std::to_string(fd);
    errno = 0;
    const ssize_t acl_bytes = path_only
        ? ::getxattr(proc_reference.c_str(), "system.posix_acl_access", nullptr, 0)
        : ::fgetxattr(fd, "system.posix_acl_access", nullptr, 0);
    if (acl_bytes >= 0) {
        (void)close_fd();
        return failure(OperationStatus::UnsupportedCapability, OperationPhase::Preflight, source,
                       "This item has an extended POSIX ACL; use a qualified ACL editor.");
    }
    if (errno != ENODATA && errno != ENOTSUP && errno != EOPNOTSUPP) {
        const std::error_code error(errno, std::generic_category());
        (void)close_fd();
        return failure(status_for_error(error), OperationPhase::Preflight, source,
                       "The item's ACL policy could not be established safely: " + error.message());
    }
    if ((static_cast<unsigned int>(before.st_mode) & 0777U) == permissions) {
        (void)close_fd();
        return OperationResult{OperationStatus::Success, OperationPhase::Complete,
                               source, "Permissions were already set to the requested mode.", false, {}};
    }

    // fchmodat2(AT_EMPTY_PATH) operates on the held inode, including when
    // mode-000 prevents opening it for reading. Never use an unsafe pathname
    // fallback on older kernels: the operation remains explicitly unsupported.
    int chmod_result = -1;
    if (path_only) {
#ifdef SYS_fchmodat2
        chmod_result = static_cast<int>(::syscall(SYS_fchmodat2, fd, "",
                                                  static_cast<mode_t>(permissions), AT_EMPTY_PATH));
#else
        errno = ENOSYS;
#endif
    } else {
        chmod_result = ::fchmod(fd, static_cast<mode_t>(permissions));
    }
    if (chmod_result != 0) {
        const std::error_code error(errno, std::generic_category());
        (void)close_fd();
        return failure(status_for_error(error), OperationPhase::Execute, source,
                       "Could not change the selected item's permissions: " + error.message());
    }
    // After fchmod the object may have changed. A failed barrier is never
    // reported as success or rolled back against an independently changed inode.
    // An O_PATH descriptor cannot be synced. Reopen through the *held* fd,
    // never the source pathname, then prove that this is still the same inode
    // before claiming durable completion.
    int sync_fd = fd;
    if (path_only) {
        sync_fd = ::open(proc_reference.c_str(), O_RDONLY | O_CLOEXEC);
        struct stat held{};
        if (sync_fd < 0 || ::fstat(sync_fd, &held) != 0 ||
            held.st_dev != before.st_dev || held.st_ino != before.st_ino) {
            if (sync_fd >= 0) ::close(sync_fd);
            (void)close_fd();
            return failure(OperationStatus::VerificationFailure, OperationPhase::Verify, source,
                           "Permissions changed, but the selected inode could not be reopened for durability verification.", true);
        }
    }
    const int sync_result = ::fsync(sync_fd);
    const int sync_errno = errno;
    if (path_only) ::close(sync_fd);
    if (sync_result != 0) {
        errno = sync_errno;
        const std::error_code error(errno, std::generic_category());
        (void)close_fd();
        return failure(OperationStatus::VerificationFailure, OperationPhase::Verify, source,
                       "Permissions changed, but metadata durability failed: " + error.message(),
                       true);
    }
    struct stat after {};
    if (::fstat(fd, &after) != 0 ||
        (static_cast<unsigned int>(after.st_mode) & 0777U) != permissions ||
        after.st_dev != before.st_dev || after.st_ino != before.st_ino) {
        (void)close_fd();
        return failure(OperationStatus::VerificationFailure, OperationPhase::Verify, source,
                       "Permissions changed but could not be verified on the original inode.", true);
    }
    if (close_fd() != 0) {
        const std::error_code error(errno, std::generic_category());
        return failure(OperationStatus::VerificationFailure, OperationPhase::Verify, source,
                       "Permissions changed, but closing the descriptor failed: " + error.message(),
                       true);
    }
    return OperationResult{OperationStatus::Success, OperationPhase::Complete, source,
                           "Updated permissions on “" + source.filename().string() + "”.", true, {}};
}

OperationResult OperationEngine::create_empty_file(const std::filesystem::path &parent,
                                                    const std::string_view name) const
{
    return create_sparse_file(parent, name, 0U);
}

OperationResult OperationEngine::create_sparse_file(const std::filesystem::path &parent,
                                                     const std::string_view name,
                                                     const std::uint64_t length) const
{
    constexpr std::uint64_t kMaxSparseBytes = 1ULL << 40U; // 1 TiB logical cap.
    if (length > kMaxSparseBytes)
        return failure(OperationStatus::InvalidRequest, OperationPhase::Preflight, {},
                       "Requested sparse file exceeds the 1 TiB logical-size limit.");
    std::string reason;
    if (!validate_item_name(name, &reason))
        return failure(OperationStatus::InvalidRequest, OperationPhase::Preflight, {},
                       std::move(reason));
    if (parent.empty())
        return failure(OperationStatus::InvalidRequest, OperationPhase::Preflight, {},
                       "The destination folder is not available.");

    const auto destination = parent / std::filesystem::path(std::string(name));
    std::error_code error;
    const auto parent_status = std::filesystem::status(parent, error);
    if (error || !std::filesystem::is_directory(parent_status))
        return failure(error ? status_for_error(error) : OperationStatus::InvalidRequest,
                       OperationPhase::Preflight, destination,
                       "The destination folder is not accessible.");
    if (path_present(destination, error))
        return failure(OperationStatus::DestinationConflict, OperationPhase::Preflight,
                       destination, "An item with that name already exists.");
    if (error)
        return failure(status_for_error(error), OperationPhase::Preflight, destination,
                       "The destination could not be checked.");

    // The same descriptor-rooted, no-replace publication boundary as New Folder.
    // An empty file is never constructed directly in the visible namespace.
    destination_ownership::ProtectedStaging staging;
    destination_ownership::OwnedOutputs owned;
    if (!destination_ownership::create_protected_staging(parent, staging, error))
        return failure(status_for_error(error), OperationPhase::Execute, destination,
                       "The private file staging area could not be created: " + error.message());

    const int fd = ::openat(staging.descriptor, "payload",
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0666);
    if (fd < 0) {
        error = std::error_code(errno, std::generic_category());
        return failure(status_for_error(error), OperationPhase::Execute, destination,
                       "The new file could not be staged: " + error.message());
    }
    const auto private_path = staging.access_root() / "payload";
    // Record ownership while the descriptor is live, before any fallible flush
    // or close. Every exit then has the same cleanup/retained-evidence contract.
    const bool recorded = destination_ownership::record_fd(private_path, fd, owned, error);
    int flushed = -1;
    // ftruncate changes logical length without allocating/writing the range.
    // Ownership was recorded before this fallible step, so no partial output
    // can be published or cleaned up under an unrelated identity.
    const bool resized = recorded && length <= static_cast<std::uint64_t>(INT64_MAX) &&
        (length == 0U || ::ftruncate(fd, static_cast<off_t>(length)) == 0);
    if (!resized && recorded) error = std::error_code(errno, std::generic_category());
    if (resized) {
        do { flushed = ::fsync(fd); } while (flushed != 0 && errno == EINTR);
        if (flushed != 0) error = std::error_code(errno, std::generic_category());
    }
    const int closed = ::close(fd);
    if (closed != 0 && !error) error = std::error_code(errno, std::generic_category());
    if (!recorded || flushed != 0 || closed != 0) {
        std::error_code cleanup;
        std::vector<std::filesystem::path> retained;
        const bool cleaned = destination_ownership::cleanup_unpublished(
            staging, destination, owned, cleanup, &retained);
        return failure(OperationStatus::VerificationFailure, OperationPhase::Verify,
                       destination, "The new file could not be made durable: " + error.message() +
                           (cleaned ? std::string{} : retained_paths_message(retained)),
                       !cleaned, std::move(retained));
    }
    if (!destination_ownership::publish_private_tree(
            staging, private_path, destination, owned, error)) {
        const auto status = status_for_error(error);
        std::error_code cleanup;
        std::vector<std::filesystem::path> retained;
        const bool cleaned = destination_ownership::cleanup_unpublished(
            staging, destination, owned, cleanup, &retained);
        return failure(status, OperationPhase::Execute, destination,
                       (status == OperationStatus::DestinationConflict
                           ? "An item with that name already exists."
                           : "The file could not be published safely: " + error.message()) +
                       (cleaned ? std::string{} : retained_paths_message(retained)),
                       !cleaned, std::move(retained));
    }
    if (!destination_ownership::verify_all(owned, error))
        return failure(OperationStatus::VerificationFailure, OperationPhase::Verify,
                       destination, "The published file could not be verified.", true);
    std::error_code verify_error;
    const std::uintmax_t actual_length = std::filesystem::file_size(destination, verify_error);
    if (verify_error || actual_length != length)
        return failure(OperationStatus::VerificationFailure, OperationPhase::Verify, destination,
                       "The created file's logical length could not be verified.", true);
    return OperationResult{OperationStatus::Success, OperationPhase::Verify, destination,
                           std::string(length == 0U ? "Created empty file “" : "Created sparse file “") +
                               destination.filename().string() + "”.", true, {}};
}

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

OperationResult OperationEngine::create_symbolic_link(
    const std::filesystem::path &source, const std::string_view name) const
{
    std::string reason;
    if (!source.is_absolute() || !validate_item_name(name, &reason))
        return failure(OperationStatus::InvalidRequest, OperationPhase::Preflight, source,
                       reason.empty() ? "Link source must be an absolute native path." : reason);
    const auto parent = source.parent_path();
    const auto destination = parent / std::string(name);
    const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) {
        const std::error_code error(errno, std::generic_category());
        return failure(status_for_error(error), OperationPhase::Preflight, destination,
                       "Cannot open the link destination folder: " + error.message());
    }
    struct stat original{};
    if (::fstatat(directory, source.filename().c_str(), &original, AT_SYMLINK_NOFOLLOW) != 0) {
        const std::error_code error(errno, std::generic_category());
        ::close(directory);
        return failure(status_for_error(error), OperationPhase::Preflight, destination,
                       "The selected link target is unavailable: " + error.message());
    }
    // A relative target preserves the selected name without inventing a new
    // absolute dependency. A later rename can intentionally leave a dangling
    // symlink; the link is not represented as a stable object reference.
    const std::string target = source.filename().string();
    if (::symlinkat(target.c_str(), directory, std::string(name).c_str()) != 0) {
        const std::error_code error(errno, std::generic_category());
        ::close(directory);
        return failure(status_for_error(error), OperationPhase::Execute, destination,
                       error == std::errc::file_exists ? "The link name already exists." :
                       "The symbolic link could not be created: " + error.message());
    }
    std::array<char, 4096> bytes{};
    const ssize_t count = ::readlinkat(directory, std::string(name).c_str(),
                                       bytes.data(), bytes.size());
    const bool verified = count == static_cast<ssize_t>(target.size()) &&
        std::string_view(bytes.data(), count > 0 ? static_cast<std::size_t>(count) : 0U) == target;
    const bool durable = verified && ::fsync(directory) == 0;
    const int closed = ::close(directory);
    if (!durable || closed != 0)
        return failure(OperationStatus::VerificationFailure, OperationPhase::Verify, destination,
                       "Symbolic link was created, but its publication could not be durably verified.", true);
    return {OperationStatus::Success, OperationPhase::Complete, destination,
            "Created symbolic link “" + destination.filename().string() + "”.", true, {}};
}

OperationResult OperationEngine::create_hard_link(
    const std::filesystem::path &source, const std::string_view name) const
{
    std::string reason;
    if (!source.is_absolute() || !validate_item_name(name, &reason))
        return failure(OperationStatus::InvalidRequest, OperationPhase::Preflight, source,
                       reason.empty() ? "Hard-link source must be an absolute native path." : reason);
    const auto destination = source.parent_path() / std::string(name);
    // Linux AT_EMPTY_PATH links the held inode, not a pathname re-resolved
    // after preflight. Never follow source symlinks or hard-link directories.
    const int original = ::open(source.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (original < 0) {
        const std::error_code error(errno, std::generic_category());
        return failure(status_for_error(error), OperationPhase::Preflight, destination,
                       "Could not open the selected hard-link source: " + error.message());
    }
    struct stat identity{};
    if (::fstat(original, &identity) != 0 || !S_ISREG(identity.st_mode)) {
        ::close(original);
        return failure(OperationStatus::UnsupportedCapability, OperationPhase::Preflight, destination,
                       "Hard links are supported only for selected native regular files.");
    }
    const int directory = ::open(source.parent_path().c_str(),
                                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) {
        const std::error_code error(errno, std::generic_category());
        ::close(original);
        return failure(status_for_error(error), OperationPhase::Preflight, destination,
                       "Could not open the hard-link destination folder: " + error.message());
    }
    if (::linkat(original, "", directory, std::string(name).c_str(), AT_EMPTY_PATH) != 0) {
        const std::error_code error(errno, std::generic_category());
        ::close(directory);
        ::close(original);
        return failure(status_for_error(error), OperationPhase::Execute, destination,
                       error == std::errc::file_exists ? "The hard-link name already exists." :
                       "Hard-link publication was refused: " + error.message());
    }
    struct stat actual{};
    const bool verified = ::fstatat(directory, std::string(name).c_str(),
                                     &actual, AT_SYMLINK_NOFOLLOW) == 0 &&
        actual.st_dev == identity.st_dev && actual.st_ino == identity.st_ino;
    const bool durable = verified && ::fsync(directory) == 0;
    const int closed = ::close(directory);
    ::close(original);
    if (!durable || closed != 0)
        return failure(OperationStatus::VerificationFailure, OperationPhase::Verify, destination,
                       "Hard link was created, but its original inode or durability could not be verified.", true);
    return {OperationStatus::Success, OperationPhase::Complete, destination,
            "Created hard link “" + destination.filename().string() + "”.", true, {}};
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
