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
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <string>
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
