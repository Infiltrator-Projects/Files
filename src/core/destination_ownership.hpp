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
                      std::error_code &error) noexcept
{
    const Identity identity = identity_for_fd(descriptor, error);
    if (error) {
        return false;
    }
    outputs.push_back(OwnedOutput{path, identity});
    return true;
}

// There is no generic POSIX directory equivalent of RENAME_NOREPLACE, and a
// link/unlink emulation can unlink a concurrently replaced source. If the
// platform/filesystem cannot provide atomic no-clobber rename, fail closed.
// A future provider may supply an equally strong primitive, but this layer
// must never weaken the destination-ownership contract to preserve support.
inline bool rename_no_replace(const std::filesystem::path &source,
                              const std::filesystem::path &destination,
                              std::error_code &error) noexcept
{
#if defined(__linux__) && defined(SYS_renameat2)
    if (::syscall(SYS_renameat2,
                  AT_FDCWD,
                  source.c_str(),
                  AT_FDCWD,
                  destination.c_str(),
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
    OwnedOutputs &outputs,
    std::error_code &error) noexcept
{
    static std::atomic<std::uint64_t> serial{0};
    for (std::uint32_t attempt = 0; attempt < 1024U; ++attempt) {
        const std::uint64_t id = serial.fetch_add(1U, std::memory_order_relaxed);
        const auto private_path =
            public_path.parent_path() /
            (".infiltrator-copy-" + std::to_string(static_cast<long long>(::getpid())) +
             "-" + std::to_string(id));
        if (!create_directory_exclusive(private_path, error)) {
            if (error == std::errc::file_exists) {
                continue;
            }
            return {};
        }
        if (record(private_path, outputs, error)) {
            return private_path;
        }
        std::error_code cleanup_error;
        (void)std::filesystem::remove(private_path, cleanup_error);
        return {};
    }
    error = std::make_error_code(std::errc::file_exists);
    return {};
}

inline std::filesystem::path create_private_symlink(
    const std::filesystem::path &public_path,
    const std::filesystem::path &target,
    OwnedOutputs &outputs,
    std::error_code &error) noexcept
{
    static std::atomic<std::uint64_t> serial{0};
    for (std::uint32_t attempt = 0; attempt < 1024U; ++attempt) {
        const std::uint64_t id = serial.fetch_add(1U, std::memory_order_relaxed);
        const auto private_path =
            public_path.parent_path() /
            (".infiltrator-link-" + std::to_string(static_cast<long long>(::getpid())) +
             "-" + std::to_string(id));
        std::filesystem::create_symlink(target, private_path, error);
        if (error == std::errc::file_exists) {
            error.clear();
            continue;
        }
        if (error) {
            return {};
        }
        if (record(private_path, outputs, error)) {
            return private_path;
        }
        std::error_code cleanup_error;
        (void)std::filesystem::remove(private_path, cleanup_error);
        return {};
    }
    error = std::make_error_code(std::errc::file_exists);
    return {};
}

inline bool publish_private_tree(const std::filesystem::path &private_root,
                                 const std::filesystem::path &public_root,
                                 OwnedOutputs &outputs,
                                 std::error_code &error) noexcept
{
    if (!rename_no_replace(private_root, public_root, error)) {
        return false;
    }
    for (OwnedOutput &output : outputs) {
        const auto relative = output.path.lexically_relative(private_root);
        output.path = relative.empty() || relative == "."
                          ? public_root
                          : public_root / relative;
    }
    return true;
}

inline bool publish_private_output(const std::filesystem::path &private_path,
                                   const std::filesystem::path &public_path,
                                   OwnedOutput &output,
                                   std::error_code &error) noexcept
{
    if (!rename_no_replace(private_path, public_path, error)) {
        return false;
    }
    output.path = public_path;
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

inline std::filesystem::path quarantine_owned(
    const OwnedOutput &output,
    std::error_code &error,
    std::filesystem::path *retained_quarantine = nullptr) noexcept
{
    if (retained_quarantine != nullptr) {
        retained_quarantine->clear();
    }

    std::filesystem::path quarantine = quarantine_no_replace(output.path, error);
    if (quarantine.empty()) {
        return {};
    }

    std::error_code inspect_error;
    if (same_object(quarantine, output.identity, inspect_error)) {
        error.clear();
        return quarantine;
    }

    std::error_code restore_error;
    const bool restored = restore_quarantine(quarantine, output.path, restore_error);
    if (!restored && retained_quarantine != nullptr) {
        *retained_quarantine = quarantine;
    }
    error = inspect_error ? inspect_error
                          : (restore_error ? restore_error
                                           : std::make_error_code(std::errc::state_not_recoverable));
    return {};
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

    const std::filesystem::path quarantine =
        quarantine_owned(output, error, retained_quarantine);
    if (quarantine.empty()) {
        return false;
    }

    if (rename_no_replace(quarantine, destination, error)) {
        return true;
    }

    const std::error_code move_error = error;
    std::error_code restore_error;
    if (!restore_quarantine(quarantine, output.path, restore_error)) {
        if (retained_quarantine != nullptr) {
            *retained_quarantine = quarantine;
        }
        error = restore_error;
        return false;
    }
    error = move_error;
    return false;
}

inline bool remove_owned_tree(const OwnedOutput &output,
                              std::error_code &error,
                              std::filesystem::path *retained_quarantine = nullptr) noexcept
{
    if (retained_quarantine != nullptr) {
        retained_quarantine->clear();
    }

    const std::filesystem::path quarantine =
        quarantine_owned(output, error, retained_quarantine);
    if (quarantine.empty()) {
        return false;
    }

    std::filesystem::remove_all(quarantine, error);
    if (!error) {
        return true;
    }

    const std::error_code remove_error = error;
    std::error_code presence_error;
    const auto status = std::filesystem::symlink_status(quarantine, presence_error);
    const bool remains = !presence_error && status.type() != std::filesystem::file_type::not_found;
    if (!remains) {
        error = remove_error;
        return false;
    }

    std::error_code restore_error;
    if (restore_quarantine(quarantine, output.path, restore_error)) {
        error = remove_error;
        return false;
    }

    if (retained_quarantine != nullptr) {
        *retained_quarantine = quarantine;
    }
    error = restore_error ? restore_error : remove_error;
    return false;
}

inline bool cleanup_owned(OwnedOutputs &outputs, std::error_code &first_error) noexcept
{
    bool complete = true;
    first_error.clear();

    for (auto iterator = outputs.rbegin(); iterator != outputs.rend(); ++iterator) {
        std::error_code quarantine_error;
        const auto quarantine = quarantine_no_replace(iterator->path, quarantine_error);
        if (quarantine.empty()) {
            if (quarantine_error == std::errc::no_such_file_or_directory ||
                quarantine_error == std::errc::not_a_directory) {
                continue;
            }
            if (quarantine_error && !first_error) {
                first_error = quarantine_error;
            }
            complete = false;
            continue;
        }

        std::error_code inspect_error;
        const bool owned = same_object(quarantine, iterator->identity, inspect_error);
        if (!owned || inspect_error) {
            std::error_code restore_error;
            if (!restore_quarantine(quarantine, iterator->path, restore_error) &&
                restore_error && !first_error) {
                first_error = restore_error;
            }
            if (inspect_error && !first_error) {
                first_error = inspect_error;
            }
            complete = false;
            continue;
        }

        std::error_code remove_error;
        const bool removed = std::filesystem::remove(quarantine, remove_error);
        if (!removed || remove_error) {
            std::error_code restore_error;
            if (!restore_quarantine(quarantine, iterator->path, restore_error) &&
                restore_error && !first_error) {
                first_error = restore_error;
            }
            if (remove_error && !first_error) {
                first_error = remove_error;
            }
            complete = false;
        }
    }

    return complete;
}

// Private copy/link paths have never been published. When a provider rejects
// atomic no-replace, the normal quarantine-based cleanup is unavailable too;
// remove only entries whose recorded identity still matches, children first.
inline bool cleanup_unpublished(OwnedOutputs &outputs, std::error_code &first_error) noexcept
{
    bool complete = true;
    first_error.clear();
    for (auto iterator = outputs.rbegin(); iterator != outputs.rend(); ++iterator) {
        std::error_code inspect_error;
        if (!same_object(iterator->path, iterator->identity, inspect_error)) {
            if (inspect_error == std::errc::no_such_file_or_directory) {
                continue;
            }
            if (inspect_error && !first_error) {
                first_error = inspect_error;
            }
            complete = false;
            continue;
        }
        std::error_code remove_error;
        if (!std::filesystem::remove(iterator->path, remove_error) || remove_error) {
            if (remove_error && !first_error) {
                first_error = remove_error;
            }
            complete = false;
        }
    }
    return complete;
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
