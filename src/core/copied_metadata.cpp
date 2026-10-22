// SPDX-License-Identifier: GPL-3.0-or-later
#include "copied_metadata.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <sys/xattr.h>
#include <vector>

namespace infiltrator::files {
namespace {
struct Attribute { std::string name; std::vector<char> value; };
constexpr std::size_t kMaximumAttributeList = 64U * 1024U;
constexpr std::size_t kMaximumAttributeValue = 64U * 1024U;
constexpr std::size_t kMaximumMetadataBytes = 4U * 1024U * 1024U;

bool attribute_absent(const int code)
{
    return code == ENODATA || code == ENOTSUP;
}

bool read_attributes(const std::filesystem::path &source,
                     std::vector<Attribute> &attributes, std::error_code &error)
{
    std::array<char, kMaximumAttributeList> names{};
    const ssize_t count = ::llistxattr(source.c_str(), names.data(), names.size());
    if (count < 0) {
        if (errno == ENOTSUP) return true; // The source has no xattr facility.
        error = std::error_code(errno, std::generic_category());
        return false;
    }
    const auto length = static_cast<std::size_t>(count);
    std::size_t total = length;
    for (std::size_t offset = 0U; offset < length;) {
        const char *name = names.data() + offset;
        const void *end = std::memchr(name, '\0', length - offset);
        if (end == nullptr || name[0] == '\0') {
            error = std::make_error_code(std::errc::io_error);
            return false;
        }
        const ssize_t size = ::lgetxattr(source.c_str(), name, nullptr, 0U);
        if (size < 0) {
            error = std::error_code(errno, std::generic_category());
            return false; // Includes an attribute removed during enumeration.
        }
        const auto bytes = static_cast<std::size_t>(size);
        if (bytes > kMaximumAttributeValue || bytes > kMaximumMetadataBytes - total) {
            error = std::make_error_code(std::errc::value_too_large);
            return false;
        }
        Attribute attribute{name, std::vector<char>(bytes)};
        const ssize_t actual = ::lgetxattr(source.c_str(), name,
                                          attribute.value.data(), bytes);
        if (actual < 0 || actual != size) {
            error = actual < 0 ? std::error_code(errno, std::generic_category())
                               : std::make_error_code(std::errc::state_not_recoverable);
            return false;
        }
        total += bytes;
        offset += attribute.name.size() + 1U;
        attributes.push_back(std::move(attribute));
    }
    return true;
}

bool is_acl(const std::string &name)
{
    return name == "system.posix_acl_access" || name == "system.posix_acl_default";
}

bool verify_attribute(const int fd, const Attribute &attribute, std::error_code &error)
{
    std::vector<char> value(attribute.value.size());
    const ssize_t count = ::fgetxattr(fd, attribute.name.c_str(), value.data(), value.size());
    if (count >= 0 && static_cast<std::size_t>(count) == attribute.value.size() &&
        value == attribute.value) return true;
    error = count < 0 ? std::error_code(errno, std::generic_category())
                      : std::make_error_code(std::errc::state_not_recoverable);
    return false;
}
} // namespace

bool sync_copied_descriptor(const int descriptor, std::error_code &error)
{
    error.clear();
    int result;
    do { result = ::fsync(descriptor); } while (result != 0 && errno == EINTR);
    if (result == 0) return true;
    error = std::error_code(errno, std::generic_category());
    return false;
}

bool sync_copy_publication(const std::filesystem::path &parent, std::error_code &error)
{
    const int fd = ::open(parent.empty() ? "." : parent.c_str(),
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) { error = std::error_code(errno, std::generic_category()); return false; }
    bool ok = sync_copied_descriptor(fd, error);
    if (::close(fd) != 0 && ok) {
        error = std::error_code(errno, std::generic_category());
        ok = false;
    }
    return ok;
}

bool preserve_copied_metadata(const std::filesystem::path &source,
                              const destination_ownership::OwnedOutput &output,
                              std::error_code &error)
{
    error.clear();
    struct stat original {};
    if (::lstat(source.c_str(), &original) != 0) {
        error = std::error_code(errno, std::generic_category());
        return false;
    }
    std::vector<Attribute> attributes;
    if (!read_attributes(source, attributes, error)) return false;
    if (S_ISLNK(original.st_mode)) {
        // Linux cannot fsetxattr an O_PATH symlink descriptor. Do not follow
        // links or mutate a separately observed replacement to emulate that
        // authority. Link entries are flushed through their containing dirs.
        if (attributes.empty()) return true;
        error = std::make_error_code(std::errc::operation_not_supported);
        return false;
    }
    const int fd = ::open(output.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
                          (S_ISDIR(original.st_mode) ? O_DIRECTORY : 0));
    if (fd < 0) { error = std::error_code(errno, std::generic_category()); return false; }
    std::error_code identity_error;
    const auto identity = destination_ownership::identity_for_fd(fd, identity_error);
    bool ok = !identity_error && identity.valid && output.identity.valid &&
              identity.device == output.identity.device && identity.inode == output.identity.inode;
    if (!ok) error = identity_error ? identity_error
                                    : std::make_error_code(std::errc::state_not_recoverable);

    // Remove inherited ACLs absent at the source; otherwise a destination
    // parent's named-user/default entries could grant access the source lacks.
    for (const char *name : {"system.posix_acl_access", "system.posix_acl_default"}) {
        if (!ok) break;
        if (std::strcmp(name, "system.posix_acl_default") == 0 && !S_ISDIR(original.st_mode))
            continue; // Default ACL operations are only defined on directories.
        const bool present = std::any_of(attributes.begin(), attributes.end(),
            [name](const Attribute &attribute) { return attribute.name == name; });
        if (!present && ::fremovexattr(fd, name) != 0 && !attribute_absent(errno)) {
            error = std::error_code(errno, std::generic_category()); ok = false;
        }
    }
    // User attributes are written while the private file is still writable.
    // Apply ACLs after chmod, which is allowed to adjust the ACL mask.
    for (const auto &attribute : attributes) {
        if (!ok) break;
        if (!is_acl(attribute.name) &&
            (::fsetxattr(fd, attribute.name.c_str(), attribute.value.data(), attribute.value.size(), 0) != 0 ||
             !verify_attribute(fd, attribute, error))) {
            if (!error) error = std::error_code(errno, std::generic_category());
            ok = false;
        }
    }
    if (ok && ::fchmod(fd, original.st_mode & 07777U) != 0) {
        error = std::error_code(errno, std::generic_category()); ok = false;
    }
    for (const auto &attribute : attributes) {
        if (!ok) break;
        if (is_acl(attribute.name) &&
            (::fsetxattr(fd, attribute.name.c_str(), attribute.value.data(), attribute.value.size(), 0) != 0 ||
             !verify_attribute(fd, attribute, error))) {
            if (!error) error = std::error_code(errno, std::generic_category());
            ok = false;
        }
    }
    const struct timespec times[2] = {{0, UTIME_OMIT}, original.st_mtim};
    if (ok && ::futimens(fd, times) != 0) {
        error = std::error_code(errno, std::generic_category()); ok = false;
    }
    struct stat preserved {};
    if (ok && (::fstat(fd, &preserved) != 0 ||
               (preserved.st_mode & 07777U) != (original.st_mode & 07777U))) {
        error = std::make_error_code(std::errc::state_not_recoverable);
        ok = false;
    }
    if (ok) ok = sync_copied_descriptor(fd, error);
    if (::close(fd) != 0 && ok) {
        error = std::error_code(errno, std::generic_category()); ok = false;
    }
    return ok;
}
} // namespace infiltrator::files
