// SPDX-License-Identifier: GPL-3.0-or-later
#include "archive_engine.hpp"
#include "transfer_operation_engine.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace infiltrator::files {
namespace {
namespace fs = std::filesystem;
constexpr std::uint64_t kMaxEntries = 50000U;
constexpr std::uint64_t kMaxFile = 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxTotal = 8ULL * 1024ULL * 1024ULL * 1024ULL;
struct ReaderCloser { void operator()(struct archive *ptr) const noexcept {
    if (ptr != nullptr) archive_read_free(ptr);
} };
using Reader = std::unique_ptr<struct archive, ReaderCloser>;
struct WriterCloser { void operator()(struct archive *p) const noexcept {
    if (p != nullptr) archive_write_free(p);
} };
using Writer = std::unique_ptr<struct archive, WriterCloser>;

OperationResult failure(OperationStatus state, OperationPhase phase,
                        const fs::path &path, std::string message)
{
    return {state, phase, path, std::move(message), false, {}};
}
bool cancelled(GCancellable *cancel)
{
    return cancel != nullptr && g_cancellable_is_cancelled(cancel);
}
std::string archive_error(struct archive *reader)
{
    const char *text = archive_error_string(reader);
    return text != nullptr ? text : "Archive provider rejected the operation.";
}
std::string folder_name(const fs::path &file)
{
    std::string text = file.filename().string();
    for (const std::string ext : {".tar.gz", ".tar.xz", ".tar.bz2", ".tar.zst",
                                   ".tgz", ".tbz", ".zip", ".tar", ".7z"}) {
        if (text.size() > ext.size() &&
            text.compare(text.size() - ext.size(), ext.size(), ext) == 0) {
            text.resize(text.size() - ext.size());
            break;
        }
    }
    return text;
}
bool safe_component_name(const std::string &name)
{
    return OperationEngine::validate_item_name(name) &&
        name.find('\\') == std::string::npos &&
        name.find(':') == std::string::npos;
}
bool valid_relative_path(const std::string &entry, fs::path &relative)
{
    if (entry.empty() || entry.size() > 4096U || entry[0] == '/' ||
        entry.find('\\') != std::string::npos ||
        entry.find(':') != std::string::npos) return false;
    relative.clear();
    std::size_t depth = 0;
    for (const fs::path &segment : fs::path(entry)) {
        const std::string part = segment.string();
        if (part.empty() || part == ".") {
            // A sole trailing slash in archive directory records is benign.
            if (part.empty()) continue;
            return false;
        }
        if (part == ".." || !safe_component_name(part) || ++depth > 64U)
            return false;
        relative /= segment;
    }
    return depth != 0U && !relative.is_absolute();
}
bool write_exact(int fd, const char *data, std::size_t length)
{
    while (length != 0U) {
        const ssize_t n = ::write(fd, data, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += n;
        length -= static_cast<std::size_t>(n);
    }
    return true;
}

struct ScopedDescriptor {
    int fd;
    explicit ScopedDescriptor(int value) : fd(value) {}
    ~ScopedDescriptor() { if (fd >= 0) ::close(fd); }
    ScopedDescriptor(const ScopedDescriptor &) = delete;
    ScopedDescriptor &operator=(const ScopedDescriptor &) = delete;
};

bool zip_header(struct archive *writer, const std::string &path, bool directory,
                std::uint64_t size)
{
    archive_entry *entry = archive_entry_new();
    if (!entry) return false;
    archive_entry_set_pathname(entry, path.c_str());
    archive_entry_set_filetype(entry, directory ? AE_IFDIR : AE_IFREG);
    archive_entry_set_perm(entry, directory ? 0755 : 0644);
    archive_entry_set_size(entry, static_cast<la_int64_t>(size));
    const bool good = archive_write_header(writer, entry) == ARCHIVE_OK;
    archive_entry_free(entry);
    return good;
}

// Children are always reopened relative to pinned parent descriptors. An
// untrusted symlink, special file or concurrently retargeted child fails the
// entire archive before publication; the source tree is never modified.
bool zip_directory(struct archive *writer, int parent_fd,
                   const std::string &prefix, unsigned depth,
                   std::uint64_t &entries, std::uint64_t &total,
                   GCancellable *cancel)
{
    if (cancelled(cancel) || depth > 64U || prefix.size() > 4096U ||
        ++entries > kMaxEntries || !zip_header(writer, prefix, true, 0U))
        return false;
    int listing_fd = ::dup(parent_fd);
    if (listing_fd < 0) return false;
    DIR *listing = ::fdopendir(listing_fd);
    if (!listing) { ::close(listing_fd); return false; }

    bool good = true;
    while (good) {
        errno = 0;
        dirent *item = ::readdir(listing);
        if (!item) {
            if (errno != 0) good = false;
            break;
        }
        const std::string name(item->d_name);
        if (name == "." || name == "..") continue;
        if (cancelled(cancel) || !safe_component_name(name) ||
            prefix.size() + name.size() > 4096U) { good = false; break; }
        struct stat observed{};
        if (::fstatat(parent_fd, name.c_str(), &observed, AT_SYMLINK_NOFOLLOW) != 0 ||
            (!S_ISDIR(observed.st_mode) && !S_ISREG(observed.st_mode))) {
            good = false; break;
        }
        const std::string member = prefix + name;
        if (S_ISDIR(observed.st_mode)) {
            ScopedDescriptor nested(::openat(parent_fd, name.c_str(),
                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            struct stat opened{};
            good = nested.fd >= 0 && ::fstat(nested.fd, &opened) == 0 &&
                opened.st_dev == observed.st_dev && opened.st_ino == observed.st_ino &&
                zip_directory(writer, nested.fd, member + "/", depth + 1U,
                              entries, total, cancel);
            continue;
        }
        if (++entries > kMaxEntries || observed.st_size < 0 ||
            static_cast<std::uint64_t>(observed.st_size) > kMaxFile ||
            total > kMaxTotal - static_cast<std::uint64_t>(observed.st_size)) {
            good = false; break;
        }
        ScopedDescriptor file(::openat(parent_fd, name.c_str(),
            O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        struct stat opened{};
        good = file.fd >= 0 && ::fstat(file.fd, &opened) == 0 &&
            S_ISREG(opened.st_mode) && opened.st_dev == observed.st_dev &&
            opened.st_ino == observed.st_ino && opened.st_size == observed.st_size &&
            zip_header(writer, member, false,
                       static_cast<std::uint64_t>(observed.st_size));
        if (!good) break;
        std::uint64_t remaining = static_cast<std::uint64_t>(observed.st_size);
        std::array<char, 65536> buffer{};
        while (good && remaining) {
            if (cancelled(cancel)) { good = false; break; }
            const ssize_t n = ::read(file.fd, buffer.data(),
                static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size())));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0 || archive_write_data(writer, buffer.data(),
                static_cast<std::size_t>(n)) != n) { good = false; break; }
            remaining -= static_cast<std::uint64_t>(n);
        }
        struct stat after{};
        if (remaining != 0U || ::fstat(file.fd, &after) != 0 ||
            after.st_size != opened.st_size ||
            after.st_mtim.tv_sec != opened.st_mtim.tv_sec ||
            after.st_mtim.tv_nsec != opened.st_mtim.tv_nsec ||
            after.st_ctim.tv_sec != opened.st_ctim.tv_sec ||
            after.st_ctim.tv_nsec != opened.st_ctim.tv_nsec)
            good = false;
        total += static_cast<std::uint64_t>(observed.st_size);
    }
    if (::closedir(listing) != 0) good = false;
    return good && !cancelled(cancel);
}

void reflect_cleanup(OperationResult &result, const fs::path &private_directory)
{
    std::error_code cleanup_error;
    fs::remove_all(private_directory, cleanup_error);
    if (cleanup_error) {
        result.changed = true;
        result.retained_paths.push_back(private_directory);
        result.message += " Private extraction staging remains: " + cleanup_error.message();
        if (result.ok()) {
            result.status = OperationStatus::VerificationFailure;
            result.phase = OperationPhase::Verify;
        }
    }
}
} // namespace


OperationResult ArchiveEngine::create_zip(const fs::path &source,
                                          const fs::path &destination_parent,
                                          const std::string &journal_id,
                                          GCancellable *cancel) const
{
    const std::string source_name = source.filename().string();
    const std::string output_name = source_name + ".zip";
    const fs::path destination = destination_parent / output_name;
    if (!source.is_absolute() || !destination_parent.is_absolute() ||
        !safe_component_name(source_name) || !safe_component_name(output_name) ||
        journal_id.empty() || journal_id.size() > 100U ||
        !std::all_of(journal_id.begin(), journal_id.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-';
        }))
        return failure(OperationStatus::InvalidRequest, OperationPhase::Preflight,
                       destination, "ZIP creation requires native paths and a valid journal ID.");
    if (cancelled(cancel))
        return failure(OperationStatus::Cancelled, OperationPhase::Preflight,
                       destination, "ZIP creation cancelled.");
    std::error_code ec;
    const fs::file_status kind = fs::symlink_status(source, ec);
    const bool directory = fs::is_directory(kind);
    if (ec || (!directory && !fs::is_regular_file(kind)))
        return failure(OperationStatus::UnsupportedCapability, OperationPhase::Preflight,
                       destination, "ZIP creation requires a native regular file or directory.");
    ec.clear();
    if (directory && fs::equivalent(source, destination_parent, ec))
        return failure(OperationStatus::InvalidRequest, OperationPhase::Preflight,
                       destination, "ZIP creation cannot place staging inside its own input.");
    if (ec)
        return failure(OperationStatus::InvalidRequest, OperationPhase::Preflight,
                       destination, "Could not validate ZIP source and destination.");
    ec.clear();
    if (!fs::is_directory(destination_parent, ec) || ec)
        return failure(OperationStatus::InvalidRequest, OperationPhase::Preflight,
                       destination, "ZIP destination is not an accessible native directory.");
    ec.clear();
    if (fs::exists(destination, ec) || ec)
        return failure(OperationStatus::DestinationConflict, OperationPhase::Preflight,
                       destination, "A ZIP with that name already exists.");
    ec.clear();
    const std::uintmax_t expected = directory ? 0U : fs::file_size(source, ec);
    if (ec || (!directory && expected > kMaxFile))
        return failure(OperationStatus::UnsupportedCapability, OperationPhase::Preflight,
                       destination, "ZIP input exceeds the qualified 1 GiB file limit.");

    std::string pattern = (destination_parent /
        (".files-archive-create-" + journal_id + "-XXXXXX")).string();
    if (::mkdtemp(pattern.data()) == nullptr)
        return failure(OperationStatus::PermissionFailure, OperationPhase::Preflight,
                       destination, "Could not allocate private ZIP staging.");
    const fs::path staging(pattern);
    const fs::path private_zip = staging / output_name;
    OperationResult result = failure(OperationStatus::ExecutionFailure,
                                     OperationPhase::Execute, destination,
                                     "ZIP creation failed.");
    Writer writer(archive_write_new());
    bool good = writer && archive_write_set_format_zip(writer.get()) == ARCHIVE_OK &&
        archive_write_open_filename(writer.get(), private_zip.c_str()) == ARCHIVE_OK;
    if (good && directory) {
        ScopedDescriptor root(::open(source.c_str(),
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        std::uint64_t entries = 0U, total = 0U;
        good = root.fd >= 0 && zip_directory(writer.get(), root.fd,
                     source_name + "/", 1U, entries, total, cancel);
    }
    if (good && !directory) {
        archive_entry *entry = archive_entry_new();
        good = entry != nullptr;
        if (good) {
            archive_entry_set_pathname(entry, source_name.c_str());
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0644);
            archive_entry_set_size(entry, static_cast<la_int64_t>(expected));
            good = archive_write_header(writer.get(), entry) == ARCHIVE_OK;
        }
        if (entry) archive_entry_free(entry);
    }
    if (good && !directory) {
        const int fd = ::open(source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        struct stat observed{};
        good = fd >= 0 && ::fstat(fd, &observed) == 0 && S_ISREG(observed.st_mode) &&
            observed.st_size >= 0 && static_cast<std::uintmax_t>(observed.st_size) == expected;
        std::array<char, 65536> bytes{};
        std::uintmax_t written = 0U;
        while (good && written < expected) {
            if (cancelled(cancel)) { good = false; break; }
            const std::size_t count = static_cast<std::size_t>(
                std::min<std::uintmax_t>(bytes.size(), expected - written));
            const ssize_t n = ::read(fd, bytes.data(), count);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0 || archive_write_data(writer.get(), bytes.data(),
                    static_cast<std::size_t>(n)) != n) { good = false; break; }
            written += static_cast<std::uintmax_t>(n);
        }
        if (fd >= 0 && ::close(fd) != 0) good = false;
        if (written != expected) good = false;
    }
    if (good) good = archive_write_close(writer.get()) == ARCHIVE_OK;
    writer.reset();
    if (!good || cancelled(cancel)) {
        result = failure(cancelled(cancel) ? OperationStatus::Cancelled :
                         OperationStatus::VerificationFailure, OperationPhase::Verify,
                         destination, "ZIP input changed or archive writing failed; nothing published.");
    } else {
        const int fd = ::open(private_zip.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        const bool flushed = fd >= 0 && ::fsync(fd) == 0;
        if (fd >= 0) ::close(fd);
        if (!flushed)
            result = failure(OperationStatus::VerificationFailure, OperationPhase::Verify,
                             destination, "Private ZIP could not be synchronised.");
        else {
            TransferOperationEngine transfer;
            result = transfer.copy_item(private_zip, destination_parent, ConflictPolicy::Fail);
        }
    }
    reflect_cleanup(result, staging);
    return result;
}

OperationResult ArchiveEngine::extract(const fs::path &source,
                                       const fs::path &destination_parent,
                                       const std::string &journal_id,
                                       GCancellable *cancel) const
{
    const std::string output_name = folder_name(source);
    const fs::path destination = destination_parent / output_name;
    if (!source.is_absolute() || !destination_parent.is_absolute() ||
        !safe_component_name(output_name) || journal_id.empty() ||
        journal_id.size() > 100U ||
        !std::all_of(journal_id.begin(), journal_id.end(), [](char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') || ch == '-';
        })) {
        return failure(OperationStatus::InvalidRequest, OperationPhase::Preflight,
                       destination, "Archive extraction requires native paths and valid journal identity.");
    }
    if (cancelled(cancel))
        return failure(OperationStatus::Cancelled, OperationPhase::Preflight,
                       destination, "Archive extraction cancelled.");
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(source, ec)) || ec)
        return failure(OperationStatus::UnsupportedCapability, OperationPhase::Preflight,
                       destination, "Only native regular archive files can be extracted.");
    if (!fs::is_directory(destination_parent, ec) || ec)
        return failure(OperationStatus::InvalidRequest, OperationPhase::Preflight,
                       destination, "Archive destination is not an accessible native directory.");
    if (fs::exists(destination, ec) || ec)
        return failure(OperationStatus::DestinationConflict, OperationPhase::Preflight,
                       destination, "Extraction target already exists. No files were replaced.");

    std::string pattern = (destination_parent /
        (".files-archive-extract-" + journal_id + "-XXXXXX")).string();
    if (::mkdtemp(pattern.data()) == nullptr)
        return failure(OperationStatus::PermissionFailure, OperationPhase::Preflight,
                       destination, "Could not create private archive staging: " +
                       std::error_code(errno, std::generic_category()).message());
    const fs::path temporary(pattern);
    const fs::path staged_root = temporary / output_name;
    OperationResult result = failure(OperationStatus::ExecutionFailure,
                                     OperationPhase::Execute, destination,
                                     "Archive extraction did not complete.");
    if (!fs::create_directory(staged_root, ec) || ec) {
        result.message = "Could not create private extraction root: " + ec.message();
        reflect_cleanup(result, temporary);
        return result;
    }

    Reader reader(archive_read_new());
    if (!reader) {
        result.message = "Could not allocate an archive reader.";
        reflect_cleanup(result, temporary);
        return result;
    }
    archive_read_support_filter_all(reader.get());
    archive_read_support_format_all(reader.get());
    if (archive_read_open_filename(reader.get(), source.c_str(), 65536U) != ARCHIVE_OK) {
        result.message = "Cannot open archive: " + archive_error(reader.get());
        reflect_cleanup(result, temporary);
        return result;
    }
    std::uint64_t count = 0U, total = 0U;
    std::array<char, 65536> buffer{};
    bool success = true;
    while (true) {
        if (cancelled(cancel)) {
            result = failure(OperationStatus::Cancelled, OperationPhase::Execute,
                             destination, "Archive extraction cancelled; no destination published.");
            success = false; break;
        }
        archive_entry *entry = nullptr;
        const int status = archive_read_next_header(reader.get(), &entry);
        if (status == ARCHIVE_EOF) break;
        if (status != ARCHIVE_OK || entry == nullptr) {
            result.message = "Archive header was rejected: " + archive_error(reader.get());
            success = false; break;
        }
        if (++count > kMaxEntries) {
            result.message = "Archive entry count exceeds the qualified limit.";
            success = false; break;
        }
        const char *name = archive_entry_pathname(entry);
        fs::path relative;
        if (name == nullptr || !valid_relative_path(name, relative)) {
            result.message = "Archive contains an unsafe or unsupported entry path.";
            success = false; break;
        }
        if (archive_entry_hardlink(entry) != nullptr ||
            archive_entry_symlink(entry) != nullptr ||
            (archive_entry_filetype(entry) != AE_IFDIR &&
             archive_entry_filetype(entry) != AE_IFREG)) {
            result.message = "Archive contains a link, device or other unsafe entry type.";
            success = false; break;
        }
        const fs::path target = staged_root / relative;
        ec.clear();
        if (!fs::create_directories(target.parent_path(), ec) && ec) {
            result.message = "Could not create private entry parent: " + ec.message();
            success = false; break;
        }
        if (archive_entry_filetype(entry) == AE_IFDIR) {
            ec.clear();
            if (!fs::create_directory(target, ec) && ec) {
                result.message = "Cannot create archive directory: " + ec.message();
                success = false; break;
            }
            continue;
        }
        const la_int64_t size = archive_entry_size(entry);
        if (size < 0 || static_cast<std::uint64_t>(size) > kMaxFile ||
            total > kMaxTotal - static_cast<std::uint64_t>(size)) {
            result.message = "Archive expanded size exceeds the safe extraction budget.";
            success = false; break;
        }
        const int fd = ::open(target.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) {
            result.message = "Archive entry duplicates another name or cannot be created: " +
                std::error_code(errno, std::generic_category()).message();
            success = false; break;
        }
        std::uint64_t written = 0U;
        bool good = true;
        while (true) {
            if (cancelled(cancel)) { good = false; break; }
            const la_ssize_t read_size = archive_read_data(reader.get(), buffer.data(), buffer.size());
            if (read_size < 0) { good = false; break; }
            if (read_size == 0) break;
            const auto bytes = static_cast<std::uint64_t>(read_size);
            if (written > static_cast<std::uint64_t>(size) ||
                bytes > static_cast<std::uint64_t>(size) - written ||
                !write_exact(fd, buffer.data(), static_cast<std::size_t>(read_size))) {
                good = false; break;
            }
            written += bytes;
        }
        if (written != static_cast<std::uint64_t>(size)) good = false;
        if (good && ::fsync(fd) != 0) good = false;
        if (::close(fd) != 0) good = false;
        if (!good) {
            result = failure(cancelled(cancel) ? OperationStatus::Cancelled :
                             OperationStatus::VerificationFailure, OperationPhase::Verify,
                             destination, "Archive entry data, checksum, or output verification failed.");
            success = false; break;
        }
        total += written;
    }
    // Close the provider reader before calling the publishing engine.
    reader.reset();
    if (success) {
        if (count == 0U) {
            result = failure(OperationStatus::UnsupportedCapability, OperationPhase::Verify,
                             destination, "An empty archive has no files to extract.");
        } else {
            TransferOperationEngine transfer;
            result = transfer.copy_item(staged_root, destination_parent,
                                        ConflictPolicy::Fail);
        }
    }
    reflect_cleanup(result, temporary);
    return result;
}

} // namespace infiltrator::files
