// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdlib.h>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1U << 0)
#endif

namespace infiltrator::files::destination_ownership {

struct Identity {
    std::uintmax_t device{0};
    std::uintmax_t inode{0};
    bool valid{false};
};

struct OwnedOutput {
    std::filesystem::path path;
    Identity identity;
};

using OwnedOutputs = std::vector<OwnedOutput>;
using RenameNoReplaceOperation = bool (*)(const std::filesystem::path &,
                                          const std::filesystem::path &,
                                          std::error_code &) noexcept;
using IdentityForFdOperation = Identity (*)(int, std::error_code &) noexcept;
using IdentityForAtOperation = Identity (*)(int, const char *, std::error_code &) noexcept;
using RemoveAllOperation = std::uintmax_t (*)(const std::filesystem::path &,
                                               std::error_code &);
using AfterUnpublishedIdentityObservation = void (*)(int) noexcept;

struct ProtectedStaging;
inline void release_staging(ProtectedStaging &staging) noexcept;
using RetainStagedPayloadOperation = bool (*)(ProtectedStaging &,
                                              const std::filesystem::path &,
                                              std::filesystem::path &,
                                              std::error_code &) noexcept;

struct ProtectedStaging {
    std::filesystem::path container_path;
    int descriptor{-1};
    Identity identity{};

    ProtectedStaging() = default;
    ProtectedStaging(const ProtectedStaging &) = delete;
    ProtectedStaging &operator=(const ProtectedStaging &) = delete;
    ~ProtectedStaging() { release_staging(*this); }

    [[nodiscard]] std::filesystem::path access_root() const
    {
        return descriptor >= 0
                   ? std::filesystem::path{"/proc/self/fd"} / std::to_string(descriptor)
                   : std::filesystem::path{};
    }
};

inline Identity identity_for(const std::filesystem::path &path, std::error_code &error) noexcept
{
    struct stat status {};
    if (::lstat(path.c_str(), &status) != 0) {
        error = std::error_code(errno, std::generic_category());
        return {};
    }
    error.clear();
    return Identity{static_cast<std::uintmax_t>(status.st_dev),
                    static_cast<std::uintmax_t>(status.st_ino),
                    true};
}

inline Identity identity_for_fd(const int descriptor, std::error_code &error) noexcept
{
    struct stat status {};
    if (::fstat(descriptor, &status) != 0) {
        error = std::error_code(errno, std::generic_category());
        return {};
    }
    error.clear();
    return Identity{static_cast<std::uintmax_t>(status.st_dev),
                    static_cast<std::uintmax_t>(status.st_ino),
                    true};
}

inline bool same_object(const std::filesystem::path &path,
                        const Identity &expected,
                        std::error_code &error) noexcept
{
    const Identity current = identity_for(path, error);
    return !error && current.valid && expected.valid &&
           current.device == expected.device && current.inode == expected.inode;
}

inline bool record(const std::filesystem::path &path,
                   OwnedOutputs &outputs,
                   std::error_code &error) noexcept
{
    const Identity identity = identity_for(path, error);
    if (error) {
        return false;
    }
    outputs.push_back(OwnedOutput{path, identity});
    return true;
}

inline bool record_fd(const std::filesystem::path &path,
                      const int descriptor,
                      OwnedOutputs &outputs,
                      std::error_code &error,
                      IdentityForFdOperation identity_operation = identity_for_fd) noexcept
{
    const Identity identity = identity_operation(descriptor, error);
    if (error) {
        return false;
    }
    outputs.push_back(OwnedOutput{path, identity});
    return true;
}


inline Identity identity_for_at(const int directory_descriptor,
                                const char *name,
                                std::error_code &error) noexcept
{
    struct stat status {};
    if (::fstatat(directory_descriptor, name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
        error = std::error_code(errno, std::generic_category());
        return {};
    }
    error.clear();
    return Identity{static_cast<std::uintmax_t>(status.st_dev),
                    static_cast<std::uintmax_t>(status.st_ino),
                    true};
}

inline bool record_at(const int directory_descriptor,
                      const char *name,
                      const std::filesystem::path &display_path,
                      OwnedOutputs &outputs,
                      std::error_code &error,
                      IdentityForAtOperation identity_operation = identity_for_at) noexcept
{
    const Identity identity = identity_operation(directory_descriptor, name, error);
    if (error) {
        return false;
    }
    outputs.push_back(OwnedOutput{display_path, identity});
    return true;
}

inline void release_staging(ProtectedStaging &staging) noexcept
{
    const int descriptor = staging.descriptor;
    const auto container_path = staging.container_path;
    const Identity expected = staging.identity;

    staging.descriptor = -1;
    staging.container_path.clear();
    staging.identity = {};

    if (descriptor < 0) {
        return;
    }

    std::error_code inspect_error;
    if (!container_path.empty() && expected.valid &&
        same_object(container_path, expected, inspect_error)) {
        std::error_code remove_error;
        (void)std::filesystem::remove(container_path, remove_error);
    }
    // Do not chase a descriptor-resolved pathname after the staging directory
    // has moved. POSIX has no remove-by-directory-fd primitive, so a pathname
    // that was verified as the owned directory can be rebound before remove.
    // Leaving an uncertain empty directory intact is safer than mutating a
    // competing writer's replacement.
    (void)::close(descriptor);
}

// Proven payload absence is only final once the still-bound staging directory
// itself has been removed while its descriptor remains live. If a late writer
// makes the directory non-empty, leave the descriptor open so the caller can
// publish that payload by descriptor-relative no-replace rename instead of
// closing over silent residue.
inline bool release_empty_staging_if_still_bound(ProtectedStaging &staging,
                                                 std::error_code &error) noexcept
{
    if (staging.descriptor < 0 || staging.container_path.empty() ||
        !staging.identity.valid) {
        error = std::make_error_code(std::errc::state_not_recoverable);
        return false;
    }

    std::error_code inspect_error;
    if (!same_object(staging.container_path, staging.identity, inspect_error)) {
        error = inspect_error ? inspect_error
                              : std::make_error_code(std::errc::state_not_recoverable);
        return false;
    }

    std::error_code remove_error;
    const bool removed = std::filesystem::remove(staging.container_path, remove_error);
    if (!removed || remove_error) {
        error = remove_error ? remove_error
                             : std::make_error_code(std::errc::state_not_recoverable);
        return false;
    }

    const int descriptor = staging.descriptor;
    staging.descriptor = -1;
    staging.container_path.clear();
    staging.identity = {};
    (void)::close(descriptor);
    error.clear();
    return true;
}

inline bool create_protected_staging(const std::filesystem::path &parent_path,
                                     ProtectedStaging &staging,
                                     std::error_code &error) noexcept
{
    release_staging(staging);
    const std::filesystem::path parent = parent_path.empty()
                                             ? std::filesystem::path{"."}
                                             : parent_path;
    const std::string pattern =
        (parent / (".infiltrator-stage-" +
                   std::to_string(static_cast<long long>(::getpid())) + "-XXXXXX"))
            .string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    char *created = ::mkdtemp(buffer.data());
    if (created == nullptr) {
        error = std::error_code(errno, std::generic_category());
        return false;
    }

    staging.container_path = std::filesystem::path{created};
    staging.descriptor =
        ::open(staging.container_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (staging.descriptor < 0) {
        error = std::error_code(errno, std::generic_category());
        staging.container_path.clear();
        return false;
    }

    staging.identity = identity_for_fd(staging.descriptor, error);
    if (error || !staging.identity.valid) {
        release_staging(staging);
        return false;
    }

    std::error_code verify_error;
    if (!same_object(staging.container_path, staging.identity, verify_error)) {
        const std::error_code result = verify_error
                                           ? verify_error
                                           : std::make_error_code(std::errc::state_not_recoverable);
        (void)::close(staging.descriptor);
        staging.descriptor = -1;
        staging.container_path.clear();
        staging.identity = {};
        error = result;
        return false;
    }

    error.clear();
    return true;
}

// There is no generic POSIX directory equivalent of RENAME_NOREPLACE, and a
// link/unlink emulation can unlink a concurrently replaced source. If the
// platform/filesystem cannot provide atomic no-clobber rename, fail closed.
// A future provider may supply an equally strong primitive, but this layer
// must never weaken the destination-ownership contract to preserve support.
inline bool rename_no_replace_at(const int source_directory,
                                 const char *source_name,
                                 const int destination_directory,
                                 const char *destination_name,
                                 std::error_code &error) noexcept
{
#if defined(__linux__) && defined(SYS_renameat2)
    if (::syscall(SYS_renameat2,
                  source_directory,
                  source_name,
                  destination_directory,
                  destination_name,
                  RENAME_NOREPLACE) == 0) {
        error.clear();
        return true;
    }

    const int rename_error = errno;
    if (rename_error != ENOSYS && rename_error != EINVAL &&
        rename_error != EOPNOTSUPP && rename_error != ENOTSUP) {
        error = std::error_code(rename_error, std::generic_category());
        return false;
    }
#endif
    error = std::make_error_code(std::errc::operation_not_supported);
    return false;
}

inline bool rename_no_replace(const std::filesystem::path &source,
                              const std::filesystem::path &destination,
                              std::error_code &error) noexcept
{
    return rename_no_replace_at(
        AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(), error);
}

inline bool retain_staged_payload(ProtectedStaging &staging,
                                  const std::filesystem::path &near_path,
                                  std::filesystem::path &retained,
                                  std::error_code &error) noexcept
{
    retained.clear();
    if (staging.descriptor < 0) {
        error = std::make_error_code(std::errc::invalid_argument);
        return false;
    }

    static std::atomic<std::uint64_t> serial{0};
    const std::filesystem::path parent = near_path.parent_path().empty()
                                             ? std::filesystem::path{"."}
                                             : near_path.parent_path();
    for (std::uint32_t attempt = 0; attempt < 1024U; ++attempt) {
        const std::uint64_t id = serial.fetch_add(1U, std::memory_order_relaxed);
        const auto candidate =
            parent /
            (".infiltrator-retained-" +
             std::to_string(static_cast<long long>(::getpid())) + "-" +
             std::to_string(id));
        if (rename_no_replace_at(staging.descriptor,
                                 "payload",
                                 AT_FDCWD,
                                 candidate.c_str(),
                                 error)) {
            retained = candidate;
            release_staging(staging);
            return true;
        }
        if (error != std::errc::file_exists) {
            return false;
        }
    }
    error = std::make_error_code(std::errc::file_exists);
    return false;
}

inline bool finalize_staged_payload_absence(ProtectedStaging &staging,
                                            const std::filesystem::path &near_path,
                                            const std::error_code &absence_error,
                                            std::filesystem::path &retained,
                                            bool &payload_absent,
                                            std::error_code &error) noexcept
{
    retained.clear();
    payload_absent = false;

    // A late writer can race an ENOENT observation. Alternate an attempted
    // empty-container release with descriptor-relative publication. Either the
    // owned empty directory is removed (absence becomes final), or any late
    // payload is moved to a stable no-replace recovery pathname before the
    // descriptor is closed. A relocated/otherwise uncertain container fails
    // closed rather than being removed through a mutable pathname.
    for (std::uint32_t attempt = 0; attempt < 16U; ++attempt) {
        std::error_code release_error;
        if (release_empty_staging_if_still_bound(staging, release_error)) {
            payload_absent = true;
            error = absence_error;
            return true;
        }

        std::error_code publication_error;
        if (retain_staged_payload(staging, near_path, retained, publication_error)) {
            error = absence_error;
            return true;
        }
        if (publication_error != std::errc::no_such_file_or_directory &&
            publication_error != std::errc::not_a_directory) {
            error = publication_error;
            return false;
        }
        if (release_error && release_error != std::errc::directory_not_empty) {
            error = release_error;
            return false;
        }
    }

    error = std::make_error_code(std::errc::state_not_recoverable);
    return false;
}

// A failed publication attempt does not prove whether the descriptor-relative
// payload still exists. Resolve absence through the open staging directory,
// then atomically publish any current entry from that descriptor to a fresh,
// no-replace ordinary pathname. No separately verified mutable pathname is
// handed to callers, and ENOENT is only finalized after the empty staging
// directory itself is safely removed.
inline bool publish_staged_payload_evidence(
    ProtectedStaging &staging,
    const std::filesystem::path &near_path,
    std::filesystem::path &retained,
    bool &payload_absent,
    std::error_code &error,
    IdentityForAtOperation identity_operation = identity_for_at) noexcept
{
    retained.clear();
    payload_absent = false;

    std::error_code presence_error;
    (void)identity_operation(staging.descriptor, "payload", presence_error);
    if (presence_error == std::errc::no_such_file_or_directory ||
        presence_error == std::errc::not_a_directory) {
        return finalize_staged_payload_absence(staging,
                                               near_path,
                                               presence_error,
                                               retained,
                                               payload_absent,
                                               error);
    }

    std::error_code publication_error;
    if (retain_staged_payload(staging, near_path, retained, publication_error)) {
        error = presence_error;
        return true;
    }
    if (publication_error == std::errc::no_such_file_or_directory ||
        publication_error == std::errc::not_a_directory) {
        return finalize_staged_payload_absence(staging,
                                               near_path,
                                               publication_error,
                                               retained,
                                               payload_absent,
                                               error);
    }

    error = presence_error ? presence_error : publication_error;
    return false;
}

inline bool create_directory_exclusive(const std::filesystem::path &path,
                                       std::error_code &error) noexcept
{
    const bool created = std::filesystem::create_directory(path, error);
    if (!error && !created) {
        error = std::make_error_code(std::errc::file_exists);
    }
    return created && !error;
}

inline std::filesystem::path quarantine_no_replace(const std::filesystem::path &path,
                                                   std::error_code &error) noexcept
{
    static std::atomic<std::uint64_t> serial{0};
    for (std::uint32_t attempt = 0; attempt < 1024U; ++attempt) {
        const std::uint64_t id = serial.fetch_add(1U, std::memory_order_relaxed);
        const auto quarantine =
            path.parent_path() /
            (".infiltrator-cleanup-" + std::to_string(static_cast<long long>(::getpid())) +
             "-" + std::to_string(id));
        if (rename_no_replace(path, quarantine, error)) {
            return quarantine;
        }
        if (error != std::errc::file_exists) {
            return {};
        }
    }
    error = std::make_error_code(std::errc::file_exists);
    return {};
}

inline std::filesystem::path create_private_directory(
    const std::filesystem::path &public_path,
    ProtectedStaging &staging,
    OwnedOutputs &outputs,
    std::error_code &error,
    IdentityForAtOperation identity_operation = identity_for_at) noexcept
{
    if (!create_protected_staging(public_path.parent_path(), staging, error)) {
        return {};
    }
    if (::mkdirat(staging.descriptor, "payload", 0700) != 0) {
        error = std::error_code(errno, std::generic_category());
        return {};
    }

    const auto private_path = staging.access_root() / "payload";
    if (record_at(staging.descriptor,
                  "payload",
                  private_path,
                  outputs,
                  error,
                  identity_operation)) {
        return private_path;
    }
    return {};
}

inline int create_private_file(
    const std::filesystem::path &public_path,
    ProtectedStaging &staging,
    OwnedOutputs &outputs,
    std::filesystem::path &private_path,
    std::error_code &error,
    IdentityForFdOperation identity_operation = identity_for_fd) noexcept
{
    private_path.clear();
    if (!create_protected_staging(public_path.parent_path(), staging, error)) {
        return -1;
    }
    const int descriptor =
        ::openat(staging.descriptor, "payload", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        error = std::error_code(errno, std::generic_category());
        return -1;
    }

    private_path = staging.access_root() / "payload";
    if (record_fd(private_path, descriptor, outputs, error, identity_operation)) {
        return descriptor;
    }

    const std::error_code record_error = error;
    (void)::close(descriptor);
    error = record_error;
    return -1;
}

inline std::filesystem::path create_private_symlink(
    const std::filesystem::path &public_path,
    const std::filesystem::path &target,
    ProtectedStaging &staging,
    OwnedOutputs &outputs,
    std::error_code &error,
    IdentityForAtOperation identity_operation = identity_for_at) noexcept
{
    if (!create_protected_staging(public_path.parent_path(), staging, error)) {
        return {};
    }
    if (::symlinkat(target.c_str(), staging.descriptor, "payload") != 0) {
        error = std::error_code(errno, std::generic_category());
        return {};
    }

    const auto private_path = staging.access_root() / "payload";
    if (record_at(staging.descriptor,
                  "payload",
                  private_path,
                  outputs,
                  error,
                  identity_operation)) {
        return private_path;
    }
    return {};
}

inline bool publish_private_tree(ProtectedStaging &staging,
                                 const std::filesystem::path &private_root,
                                 const std::filesystem::path &public_root,
                                 OwnedOutputs &outputs,
                                 std::error_code &error) noexcept
{
    if (!rename_no_replace_at(staging.descriptor,
                              "payload",
                              AT_FDCWD,
                              public_root.c_str(),
                              error)) {
        return false;
    }
    for (OwnedOutput &output : outputs) {
        const auto relative = output.path.lexically_relative(private_root);
        output.path = relative.empty() || relative == "."
                          ? public_root
                          : public_root / relative;
    }
    release_staging(staging);
    return true;
}

inline bool publish_private_output(ProtectedStaging &staging,
                                   const std::filesystem::path &private_path,
                                   const std::filesystem::path &public_path,
                                   OwnedOutput &output,
                                   std::error_code &error) noexcept
{
    (void)private_path;
    if (!rename_no_replace_at(staging.descriptor,
                              "payload",
                              AT_FDCWD,
                              public_path.c_str(),
                              error)) {
        return false;
    }
    output.path = public_path;
    release_staging(staging);
    return true;
}

inline bool restore_quarantine(const std::filesystem::path &quarantine,
                               const std::filesystem::path &original,
                               std::error_code &error) noexcept
{
    return rename_no_replace(quarantine, original, error);
}

inline bool verify_all(const OwnedOutputs &outputs, std::error_code &error) noexcept
{
    error.clear();
    for (const OwnedOutput &output : outputs) {
        if (!same_object(output.path, output.identity, error)) {
            if (!error) {
                error = std::make_error_code(std::errc::state_not_recoverable);
            }
            return false;
        }
    }
    return !outputs.empty();
}

inline bool quarantine_owned_with_operations(
    const OwnedOutput &output,
    ProtectedStaging &staging,
    std::filesystem::path &quarantine,
    std::error_code &error,
    std::filesystem::path *retained_quarantine,
    IdentityForAtOperation identity_operation,
    RetainStagedPayloadOperation retain_operation) noexcept
{
    quarantine.clear();
    if (retained_quarantine != nullptr) {
        retained_quarantine->clear();
    }
    if (!create_protected_staging(output.path.parent_path(), staging, error)) {
        return false;
    }
    if (!rename_no_replace_at(AT_FDCWD,
                              output.path.c_str(),
                              staging.descriptor,
                              "payload",
                              error)) {
        return false;
    }

    quarantine = staging.access_root() / "payload";
    std::error_code inspect_error;
    const Identity current = identity_operation(staging.descriptor, "payload", inspect_error);
    if (!inspect_error && current.valid && output.identity.valid &&
        current.device == output.identity.device && current.inode == output.identity.inode) {
        error.clear();
        return true;
    }

    std::error_code restore_error;
    if (rename_no_replace_at(staging.descriptor,
                             "payload",
                             AT_FDCWD,
                             output.path.c_str(),
                             restore_error)) {
        release_staging(staging);
    } else {
        std::filesystem::path retained;
        std::error_code retain_error;
        if (retain_operation(staging, output.path, retained, retain_error)) {
            if (retained_quarantine != nullptr) {
                *retained_quarantine = retained;
            }
        } else {
            bool payload_absent = false;
            std::error_code evidence_error;
            if (publish_staged_payload_evidence(staging,
                                                output.path,
                                                retained,
                                                payload_absent,
                                                evidence_error) &&
                !payload_absent && retained_quarantine != nullptr) {
                *retained_quarantine = retained;
            }
        }
    }

    error = inspect_error ? inspect_error
                          : std::make_error_code(std::errc::state_not_recoverable);
    return false;
}

inline bool quarantine_owned(
    const OwnedOutput &output,
    ProtectedStaging &staging,
    std::filesystem::path &quarantine,
    std::error_code &error,
    std::filesystem::path *retained_quarantine = nullptr) noexcept
{
    return quarantine_owned_with_operations(output,
                                            staging,
                                            quarantine,
                                            error,
                                            retained_quarantine,
                                            identity_for_at,
                                            retain_staged_payload);
}

inline bool restore_quarantine(ProtectedStaging &staging,
                               const std::filesystem::path &original,
                               std::error_code &error,
                               std::filesystem::path *retained_quarantine = nullptr,
                               bool *payload_absent = nullptr,
                               RetainStagedPayloadOperation retain_operation =
                                   retain_staged_payload,
                               IdentityForAtOperation identity_operation =
                                   identity_for_at) noexcept
{
    if (retained_quarantine != nullptr) {
        retained_quarantine->clear();
    }
    if (payload_absent != nullptr) {
        *payload_absent = false;
    }
    if (rename_no_replace_at(staging.descriptor,
                             "payload",
                             AT_FDCWD,
                             original.c_str(),
                             error)) {
        release_staging(staging);
        return true;
    }

    const std::error_code restore_error = error;
    std::filesystem::path retained;
    std::error_code retain_error;
    if (retain_operation(staging, original, retained, retain_error)) {
        if (retained_quarantine != nullptr) {
            *retained_quarantine = retained;
        }
        error = restore_error;
        return false;
    }
    bool absent = false;
    std::error_code evidence_error;
    if (publish_staged_payload_evidence(staging,
                                        original,
                                        retained,
                                        absent,
                                        evidence_error,
                                        identity_operation) &&
        !absent && retained_quarantine != nullptr) {
        *retained_quarantine = retained;
    }
    if (payload_absent != nullptr) {
        *payload_absent = absent;
    }
    error = retain_error ? retain_error : restore_error;
    return false;
}

inline bool destination_is_different_device(const std::filesystem::path &source,
                                            const std::filesystem::path &destination,
                                            bool &different,
                                            std::error_code &error) noexcept
{
    struct stat source_status {};
    struct stat parent_status {};
    const std::filesystem::path parent = destination.parent_path().empty()
                                             ? std::filesystem::path{"."}
                                             : destination.parent_path();
    if (::lstat(source.c_str(), &source_status) != 0) {
        error = std::error_code(errno, std::generic_category());
        return false;
    }
    if (::stat(parent.c_str(), &parent_status) != 0) {
        error = std::error_code(errno, std::generic_category());
        return false;
    }
    different = source_status.st_dev != parent_status.st_dev;
    error.clear();
    return true;
}

inline bool move_owned_no_replace(const OwnedOutput &source,
                                  const std::filesystem::path &destination,
                                  OwnedOutput &moved,
                                  std::error_code &error,
                                  std::filesystem::path *retained_quarantine = nullptr,
                                  RenameNoReplaceOperation rename_operation = rename_no_replace) noexcept
{
    if (retained_quarantine != nullptr) {
        retained_quarantine->clear();
    }

    bool different_device = false;
    if (!destination_is_different_device(source.path, destination, different_device, error)) {
        return false;
    }
    if (different_device) {
        error = std::make_error_code(std::errc::cross_device_link);
        return false;
    }

    static std::atomic<std::uint64_t> serial{0};
    std::filesystem::path quarantine;
    for (std::uint32_t attempt = 0; attempt < 1024U; ++attempt) {
        const std::uint64_t id = serial.fetch_add(1U, std::memory_order_relaxed);
        const auto candidate =
            source.path.parent_path() /
            (".infiltrator-cleanup-" +
             std::to_string(static_cast<long long>(::getpid())) + "-" +
             std::to_string(id));
        if (rename_operation(source.path, candidate, error)) {
            quarantine = candidate;
            break;
        }
        if (error != std::errc::file_exists) {
            return false;
        }
    }
    if (quarantine.empty()) {
        error = std::make_error_code(std::errc::file_exists);
        return false;
    }

    std::error_code inspect_error;
    if (!same_object(quarantine, source.identity, inspect_error)) {
        std::error_code restore_error;
        if (!rename_operation(quarantine, source.path, restore_error) &&
            retained_quarantine != nullptr) {
            *retained_quarantine = quarantine;
        }
        error = inspect_error ? inspect_error
                              : (restore_error ? restore_error
                                               : std::make_error_code(std::errc::state_not_recoverable));
        return false;
    }

    if (rename_operation(quarantine, destination, error)) {
        moved = OwnedOutput{destination, source.identity};
        return true;
    }

    const std::error_code publish_error = error;
    std::error_code restore_error;
    if (!rename_operation(quarantine, source.path, restore_error)) {
        if (retained_quarantine != nullptr) {
            *retained_quarantine = quarantine;
        }
        error = restore_error;
        return false;
    }
    error = publish_error;
    return false;
}

inline bool restore_owned(const OwnedOutput &output,
                          const std::filesystem::path &destination,
                          std::error_code &error,
                          std::filesystem::path *retained_quarantine = nullptr) noexcept
{
    if (retained_quarantine != nullptr) {
        retained_quarantine->clear();
    }

    ProtectedStaging staging;
    std::filesystem::path quarantine;
    if (!quarantine_owned(output, staging, quarantine, error, retained_quarantine)) {
        return false;
    }

    if (rename_no_replace_at(staging.descriptor,
                             "payload",
                             AT_FDCWD,
                             destination.c_str(),
                             error)) {
        release_staging(staging);
        return true;
    }

    const std::error_code move_error = error;
    std::error_code restore_error;
    std::filesystem::path retained;
    if (!restore_quarantine(staging, output.path, restore_error, &retained)) {
        if (retained_quarantine != nullptr) {
            *retained_quarantine = retained;
        }
        error = restore_error;
        return false;
    }
    error = move_error;
    return false;
}

inline std::uintmax_t remove_all_path(const std::filesystem::path &path,
                                     std::error_code &error)
{
    return std::filesystem::remove_all(path, error);
}

inline bool remove_owned_tree(const OwnedOutput &output,
                              std::error_code &error,
                              std::filesystem::path *retained_quarantine = nullptr,
                              RemoveAllOperation remove_operation = remove_all_path) noexcept
{
    if (retained_quarantine != nullptr) {
        retained_quarantine->clear();
    }

    ProtectedStaging staging;
    std::filesystem::path quarantine;
    if (!quarantine_owned(output, staging, quarantine, error, retained_quarantine)) {
        return false;
    }

    (void)remove_operation(quarantine, error);
    if (!error) {
        release_staging(staging);
        return true;
    }

    const std::error_code remove_error = error;
    std::error_code presence_error;
    const auto status = std::filesystem::symlink_status(quarantine, presence_error);
    const bool remains = !presence_error && status.type() != std::filesystem::file_type::not_found;
    if (!remains) {
        release_staging(staging);
        error = remove_error;
        return false;
    }

    std::error_code restore_error;
    std::filesystem::path retained;
    if (restore_quarantine(staging, output.path, restore_error, &retained)) {
        error = remove_error;
        return false;
    }

    if (retained_quarantine != nullptr) {
        *retained_quarantine = retained;
    }
    error = restore_error ? restore_error : remove_error;
    return false;
}

inline bool cleanup_owned(OwnedOutputs &outputs,
                         std::error_code &first_error,
                         std::vector<std::filesystem::path> *retained_paths = nullptr) noexcept
{
    bool complete = true;
    first_error.clear();
    if (retained_paths != nullptr) {
        retained_paths->clear();
    }

    for (auto iterator = outputs.rbegin(); iterator != outputs.rend(); ++iterator) {
        ProtectedStaging staging;
        std::filesystem::path quarantine;
        std::filesystem::path retained;
        std::error_code quarantine_error;
        if (!quarantine_owned(*iterator,
                              staging,
                              quarantine,
                              quarantine_error,
                              &retained)) {
            if (quarantine_error == std::errc::no_such_file_or_directory ||
                quarantine_error == std::errc::not_a_directory) {
                continue;
            }
            if (quarantine_error && !first_error) {
                first_error = quarantine_error;
            }
            if (retained_paths != nullptr) {
                retained_paths->push_back(retained.empty() ? iterator->path : retained);
            }
            complete = false;
            continue;
        }

        std::error_code remove_error;
        const bool removed = std::filesystem::remove(quarantine, remove_error);
        if (removed && !remove_error) {
            release_staging(staging);
            continue;
        }

        std::error_code restore_error;
        std::filesystem::path restore_retained;
        const bool restored = restore_quarantine(staging,
                                                   iterator->path,
                                                   restore_error,
                                                   &restore_retained);
        if (!restored && restore_error && !first_error) {
            first_error = restore_error;
        }
        if (retained_paths != nullptr) {
            retained_paths->push_back(
                restored ? iterator->path
                         : (restore_retained.empty() ? quarantine : restore_retained));
        }
        if (remove_error && !first_error) {
            first_error = remove_error;
        }
        complete = false;
    }

    return complete;
}

// Private copy/link paths have never been published. When a provider rejects
// atomic no-replace, the normal quarantine-based cleanup is unavailable too;
// remove only entries whose recorded identity still matches, children first.
inline bool cleanup_unpublished(
    ProtectedStaging &staging,
    const std::filesystem::path &near_path,
    OwnedOutputs &outputs,
    std::error_code &first_error,
    std::vector<std::filesystem::path> *retained_paths = nullptr,
    AfterUnpublishedIdentityObservation after_identity_observation = nullptr,
    IdentityForAtOperation payload_identity_operation = identity_for_at) noexcept
{
    first_error.clear();
    if (retained_paths != nullptr) {
        retained_paths->clear();
    }
    if (staging.descriptor < 0) {
        first_error = std::make_error_code(std::errc::bad_file_descriptor);
        return false;
    }

    // Preserve an identity observation for diagnostics and deterministic race
    // coverage, but never treat that observation as continuing authority.
    const std::filesystem::path access_root = staging.access_root();
    for (const auto &output : outputs) {
        if (output.path.lexically_relative(access_root) == "payload") {
            std::error_code ignored;
            (void)identity_for_at(staging.descriptor, "payload", ignored);
            break;
        }
    }

    // There is no POSIX primitive that conditionally unlinks a directory entry
    // by inode identity.  Re-checking immediately before unlinkat still leaves a
    // same-UID check/mutate race.  The fallback therefore publishes whichever
    // entry is current through the live directory descriptor with one atomic,
    // no-replace rename.  This hook exercises the former critical interval.
    if (after_identity_observation != nullptr) {
        after_identity_observation(staging.descriptor);
    }

    std::error_code payload_error;
    const Identity payload_identity =
        payload_identity_operation(staging.descriptor, "payload", payload_error);
    if (payload_error || !payload_identity.valid) {
        if (payload_error == std::errc::no_such_file_or_directory) {
            std::filesystem::path retained;
            bool payload_absent = false;
            std::error_code absence_error;
            if (finalize_staged_payload_absence(staging,
                                                near_path,
                                                payload_error,
                                                retained,
                                                payload_absent,
                                                absence_error) &&
                !payload_absent && retained_paths != nullptr) {
                retained_paths->push_back(retained);
                first_error = std::make_error_code(std::errc::state_not_recoverable);
                return false;
            }
            if (payload_absent) {
                return true;
            }
            first_error = absence_error ? absence_error : payload_error;
            return false;
        }
        first_error = payload_error ? payload_error
                                    : std::make_error_code(
                                          std::errc::state_not_recoverable);
        std::filesystem::path retained;
        bool payload_absent = false;
        std::error_code publication_error;
        if (publish_staged_payload_evidence(staging,
                                            near_path,
                                            retained,
                                            payload_absent,
                                            publication_error,
                                            payload_identity_operation) &&
            !payload_absent && retained_paths != nullptr) {
            retained_paths->push_back(retained);
        }
        return false;
    }

    first_error = std::make_error_code(std::errc::state_not_recoverable);
    std::filesystem::path retained;
    bool payload_absent = false;
    std::error_code publication_error;
    if (publish_staged_payload_evidence(staging,
                                        near_path,
                                        retained,
                                        payload_absent,
                                        publication_error) &&
        !payload_absent && retained_paths != nullptr) {
        retained_paths->push_back(retained);
    }
    return false;
}

inline int open_exclusive(const std::filesystem::path &path, std::error_code &error) noexcept
{
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        error = std::error_code(errno, std::generic_category());
        return -1;
    }
    error.clear();
    return descriptor;
}

} // namespace infiltrator::files::destination_ownership
