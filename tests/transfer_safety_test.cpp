// SPDX-License-Identifier: GPL-3.0-or-later
#include "transfer_operation_engine.hpp"
#include "operation_journal.hpp"
#include "batch_transfer_engine.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <stdexcept>
#include <sys/xattr.h>
#include <endian.h>

using namespace infiltrator::files;
namespace fs = std::filesystem;
namespace {
enum class Fail { None, FileFlush, ParentFlush, Attribute };
Fail failure = Fail::None;
struct stat parent_identity {};
bool file_flushed = false;
bool parent_flushed = false;
bool removed_after_barriers = false;
TransferControl *cancel_on_file_flush = nullptr;
void require(const bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
void write(const fs::path &path) { std::ofstream(path) << "transfer safety payload\n"; }
bool force_cross_device(const destination_ownership::OwnedOutput &, const fs::path &,
                        destination_ownership::OwnedOutput &, std::error_code &error,
                        fs::path *retained) noexcept
{
    if (retained != nullptr) retained->clear();
    error = std::make_error_code(std::errc::cross_device_link);
    return false;
}
void remove_after_barriers(const fs::path &path, std::error_code &error) noexcept
{
    removed_after_barriers = file_flushed && parent_flushed;
    fs::remove_all(path, error);
}
void reset(const fs::path &parent, const Fail fail = Fail::None)
{
    require(::stat(parent.c_str(), &parent_identity) == 0, "Cannot inspect destination parent");
    failure = fail;
    file_flushed = parent_flushed = removed_after_barriers = false;
}
void check_retained(const OperationResult &result)
{
    require(result.changed && !result.retained_paths.empty(), "Retained output must carry exact evidence");
    for (const auto &path : result.retained_paths)
        require(fs::exists(path), "Recovery evidence must identify existing data");
}
std::vector<char> attribute(const fs::path &path, const char *name)
{
    std::array<char, 65536> buffer{};
    const auto count = ::getxattr(path.c_str(), name, buffer.data(), buffer.size());
    require(count >= 0, "Preserved attribute is missing");
    return {buffer.begin(), buffer.begin() + count};
}
std::vector<char> named_acl(const bool directory = false)
{
    struct {
        posix_acl_xattr_header header;
        std::array<posix_acl_xattr_entry, 5> entries;
    } acl{};
    acl.header.a_version = htole32(POSIX_ACL_XATTR_VERSION);
    const std::array<unsigned short, 5> tags = {ACL_USER_OBJ, ACL_USER, ACL_GROUP_OBJ, ACL_MASK, ACL_OTHER};
    const std::array<unsigned short, 5> permissions = {static_cast<unsigned short>(directory ? 7 : 6), 4, 4, 4, 0};
    for (std::size_t i = 0U; i < tags.size(); ++i) {
        acl.entries[i].e_tag = htole16(tags[i]);
        acl.entries[i].e_perm = htole16(permissions[i]);
        acl.entries[i].e_id = htole32(i == 1U ? static_cast<std::uint32_t>(::getuid()) : 0xffffffffU);
    }
    const auto *bytes = reinterpret_cast<const char *>(&acl);
    return {bytes, bytes + sizeof acl};
}
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(const int fd)
{
    struct stat status {};
    if (::fstat(fd, &status) != 0) return -1;
    const bool parent = status.st_dev == parent_identity.st_dev && status.st_ino == parent_identity.st_ino;
    if ((failure == Fail::FileFlush && S_ISREG(status.st_mode)) ||
        (failure == Fail::ParentFlush && parent)) { errno = EIO; return -1; }
    const int result = __real_fsync(fd);
    if (result == 0) {
        if (S_ISREG(status.st_mode)) {
            file_flushed = true;
            if (cancel_on_file_flush != nullptr) cancel_on_file_flush->request_cancel();
        }
        if (parent) parent_flushed = true;
    }
    return result;
}
extern "C" int __real_fsetxattr(int, const char *, const void *, size_t, int);
extern "C" int __wrap_fsetxattr(int fd, const char *name, const void *value, size_t size, int flags)
{
    if (failure == Fail::Attribute && std::strcmp(name, "user.files-safety") == 0) {
        errno = EOPNOTSUPP; return -1;
    }
    return __real_fsetxattr(fd, name, value, size, flags);
}

int main()
{
    char pattern[] = "/tmp/files-transfer-safety-XXXXXX";
    char *created = ::mkdtemp(pattern);
    if (created == nullptr) return 2;
    const fs::path root(created), parent = root / "destination";
    fs::create_directory(parent);
    try {
        TransferOperationHooks hooks;
        hooks.move_owned = force_cross_device;
        hooks.remove_source_quarantine = remove_after_barriers;
        TransferOperationEngine engine(hooks);
        const auto source = root / "file.txt";
        write(source);
        require(::setxattr(source.c_str(), "user.files-safety", "metadata", 8U, 0) == 0,
                "Test filesystem must support user xattrs");
        require(::setxattr(source.c_str(), "user.files-empty", "", 0U, 0) == 0,
                "Test filesystem must support empty xattrs");
        const auto acl = named_acl();
        require(::setxattr(source.c_str(), "system.posix_acl_access", acl.data(), acl.size(), 0) == 0,
                "Test filesystem must support POSIX ACLs");
        const auto expected_acl = attribute(source, "system.posix_acl_access");
        reset(parent);
        auto result = engine.move_item(source, parent, ConflictPolicy::Fail);
        require(result.ok() && !fs::exists(source) && removed_after_barriers,
                "Cross-volume source removal must follow destination file and parent flushes");
        require(attribute(parent / "file.txt", "user.files-safety") == std::vector<char>({'m','e','t','a','d','a','t','a'}),
                "Move must preserve user attribute bytes");
        require(attribute(parent / "file.txt", "user.files-empty").empty(), "Empty attribute must remain present");
        require(attribute(parent / "file.txt", "system.posix_acl_access") == expected_acl,
                "Move must preserve named-user ACLs");

        for (const Fail fail : {Fail::FileFlush, Fail::ParentFlush, Fail::Attribute}) {
            const auto item = root / (fail == Fail::FileFlush ? "flush.txt" : fail == Fail::ParentFlush ? "parent.txt" : "attribute.txt");
            write(item);
            require(::setxattr(item.c_str(), "user.files-safety", "metadata", 8U, 0) == 0, "Cannot set fixture metadata");
            reset(parent, fail);
            result = engine.move_item(item, parent, ConflictPolicy::Fail);
            failure = Fail::None;
            require(result.status == OperationStatus::VerificationFailure && fs::exists(item),
                    "Flush or metadata failure must retain the original source");
            require(!removed_after_barriers, "Failed transfer must not remove its source");
            check_retained(result);
        }

        const auto cancelled_source = root / "cancelled.txt";
        write(cancelled_source);
        TransferControl control;
        reset(parent);
        cancel_on_file_flush = &control;
        result = engine.move_item(cancelled_source, parent, ConflictPolicy::Fail, &control);
        cancel_on_file_flush = nullptr;
        require(result.status == OperationStatus::Cancelled && fs::exists(cancelled_source) &&
                !fs::exists(parent / "cancelled.txt"),
                "Cancellation during durability must stop before public commit or source removal");

        // Private outputs inherit from their destination; absence of a source
        // ACL must remove inherited named/default entries, not leave a latent grant.
        const auto default_acl = named_acl(true);
        require(::setxattr(parent.c_str(), "system.posix_acl_default", default_acl.data(), default_acl.size(), 0) == 0,
                "Cannot install destination default ACL");
        const auto plain = root / "plain";
        fs::create_directory(plain);
        write(plain / "child.txt");
        reset(parent);
        result = engine.copy_item(plain, parent, ConflictPolicy::Fail);
        if (!result.ok()) std::cerr << result.message << '\n';
        require(result.ok(), "Plain directory copy must succeed below an ACL-bearing destination");
        char value[128];
        errno = 0;
        require(::getxattr((parent / "plain").c_str(), "system.posix_acl_default", value, sizeof value) == -1 && errno == ENODATA,
                "Copy must not acquire a destination-only default ACL");

        // An empty-file flush failure must either clean up or report its exact
        // retained payload, never claim unchanged while leaving anonymous data.
        const auto empty_parent = root / "empty";
        fs::create_directory(empty_parent);
        reset(empty_parent, Fail::FileFlush);
        result = OperationEngine{}.create_empty_file(empty_parent, "empty.txt");
        failure = Fail::None;
        require(result.status == OperationStatus::VerificationFailure && !fs::exists(empty_parent / "empty.txt"),
                "Failed empty-file creation must not publish the requested name");
        check_retained(result);

        const auto state = root / "state";
        fs::create_directories(state / "files");
        require(::setenv("XDG_STATE_HOME", state.c_str(), 1) == 0, "Cannot configure disposable journal");
        std::ofstream(state / "files" / "operations.log") << "invalid complete record\n";
        write(root / "blocked.txt");
        result = BatchTransferEngine{}.execute({root / "blocked.txt"}, parent,
                                               BatchTransferKind::Copy, BatchConflictPolicy::Fail);
        require(result.status == OperationStatus::VerificationFailure && !fs::exists(parent / "blocked.txt"),
                "Malformed journal must block the real batch mutation path");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        failure = Fail::None;
        fs::remove_all(root);
        return 1;
    }
    fs::remove_all(root);
    return 0;
}
