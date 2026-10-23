// SPDX-License-Identifier: GPL-3.0-or-later
#include "transfer_operation_engine.hpp"
#include "aggregate_arithmetic.hpp"
#include "copied_metadata.hpp"
#include "destination_ownership.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>
#include <glib.h>
#include <string>
#include <utility>
#include <vector>

namespace infiltrator::files {

namespace {

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
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory) {
        error.clear();
        return false;
    }
    return !error && status.type() != std::filesystem::file_type::not_found;
}

bool path_is_within(const std::filesystem::path &candidate,
                    const std::filesystem::path &ancestor,
                    std::error_code &error)
{
    error.clear();
    const auto canonical_candidate = std::filesystem::weakly_canonical(candidate, error);
    if (error) {
        return false;
    }
    const auto canonical_ancestor = std::filesystem::weakly_canonical(ancestor, error);
    if (error) {
        return false;
    }

    auto ancestor_it = canonical_ancestor.begin();
    auto candidate_it = canonical_candidate.begin();
    while (ancestor_it != canonical_ancestor.end()) {
        if (candidate_it == canonical_candidate.end() || *ancestor_it != *candidate_it) {
            return false;
        }
        ++ancestor_it;
        ++candidate_it;
    }
    return true;
}

// Compare actual bytes as well as names, sizes and types. This is intentionally
// used after copying and again after source quarantine for cross-volume moves.
// A snapshot/lock is still required to guarantee a stable concurrently-written
// file; detecting same-length edits avoids false verification by size alone.
bool identical_file_bytes(const std::filesystem::path &source,
                          const std::filesystem::path &destination,
                          std::error_code &error)
{
    std::ifstream left(source, std::ios::binary);
    std::ifstream right(destination, std::ios::binary);
    if (!left.is_open() || !right.is_open()) {
        error = std::make_error_code(std::errc::io_error);
        return false;
    }
    std::array<char, 65536> a{};
    std::array<char, 65536> b{};
    for (;;) {
        left.read(a.data(), static_cast<std::streamsize>(a.size()));
        right.read(b.data(), static_cast<std::streamsize>(b.size()));
        const auto left_count = left.gcount();
        const auto right_count = right.gcount();
        if (left.bad() || right.bad()) {
            error = std::make_error_code(std::errc::io_error);
            return false;
        }
        if (left_count != right_count ||
            !std::equal(a.data(), a.data() + left_count, b.data())) {
            return false;
        }
        if (left_count == 0) return true;
    }
}

using FileContentEvidence = std::vector<std::pair<std::filesystem::path, std::string>>;

// The private output is still owner-readable here, before the destination's
// original mode is applied. Retain a digest so later source verification need
// not reopen a destination which may correctly have no owner-read permission.
std::string digest_file_bytes(const std::filesystem::path &path,
                              std::error_code &error)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        error = std::make_error_code(std::errc::io_error);
        return {};
    }
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    if (checksum == nullptr) {
        error = std::make_error_code(std::errc::not_enough_memory);
        return {};
    }
    std::array<char, 65536> buffer{};
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto n = stream.gcount();
        if (n > 0)
            g_checksum_update(checksum,
                              reinterpret_cast<const guchar *>(buffer.data()),
                              static_cast<gssize>(n));
    }
    std::string digest;
    if (stream.bad() || !stream.eof()) {
        error = std::make_error_code(std::errc::io_error);
    } else {
        digest = g_checksum_get_string(checksum);
    }
    g_checksum_free(checksum);
    return digest;
}

bool matches_content_evidence(const std::filesystem::path &source_root,
                              const FileContentEvidence &evidence,
                              std::error_code &error)
{
    for (const auto &[relative, expected] : evidence) {
        // A single-file copy records "." relative to its root. Appending
        // "/." to a regular-file quarantine path would yield ENOTDIR.
        const std::filesystem::path file =
            (relative.empty() || relative == ".") ? source_root : source_root / relative;
        const std::string actual = digest_file_bytes(file, error);
        if (error || actual != expected) return false;
    }
    return true;
}

bool equivalent_copy_shape(const std::filesystem::path &source,
                           const std::filesystem::path &destination,
                           std::error_code &error)
{
    error.clear();
    const std::filesystem::file_status source_status =
        std::filesystem::symlink_status(source, error);
    if (error) {
        return false;
    }
    const std::filesystem::file_status destination_status =
        std::filesystem::symlink_status(destination, error);
    if (error || source_status.type() != destination_status.type()) {
        return false;
    }

    if (std::filesystem::is_regular_file(source_status)) {
        const std::uintmax_t source_size = std::filesystem::file_size(source, error);
        if (error) {
            return false;
        }
        const std::uintmax_t destination_size =
            std::filesystem::file_size(destination, error);
        return !error && source_size == destination_size;
    }

    if (std::filesystem::is_symlink(source_status)) {
        const std::filesystem::path source_target =
            std::filesystem::read_symlink(source, error);
        if (error) {
            return false;
        }
        const std::filesystem::path destination_target =
            std::filesystem::read_symlink(destination, error);
        return !error && source_target == destination_target;
    }

    if (!std::filesystem::is_directory(source_status)) {
        return false;
    }

    std::uintmax_t source_entries = 0U;
    for (std::filesystem::recursive_directory_iterator iterator(source, error), end;
         !error && iterator != end;
         iterator.increment(error)) {
        if (!checked_add_uintmax(1U, source_entries)) {
            error = std::make_error_code(std::errc::value_too_large);
            return false;
        }
        const std::filesystem::path relative =
            iterator->path().lexically_relative(source);
        if (relative.empty()) {
            return false;
        }

        const std::filesystem::path counterpart = destination / relative;
        const std::filesystem::file_status item_status = iterator->symlink_status(error);
        if (error) {
            return false;
        }
        const std::filesystem::file_status counterpart_status =
            std::filesystem::symlink_status(counterpart, error);
        if (error || item_status.type() != counterpart_status.type()) {
            return false;
        }
        if (std::filesystem::is_regular_file(item_status)) {
            const std::uintmax_t item_size =
                std::filesystem::file_size(iterator->path(), error);
            if (error) {
                return false;
            }
            const std::uintmax_t counterpart_size =
                std::filesystem::file_size(counterpart, error);
            if (error || item_size != counterpart_size) {
                return false;
            }
        } else if (std::filesystem::is_symlink(item_status)) {
            const std::filesystem::path item_target =
                std::filesystem::read_symlink(iterator->path(), error);
            if (error) {
                return false;
            }
            const std::filesystem::path counterpart_target =
                std::filesystem::read_symlink(counterpart, error);
            if (error || item_target != counterpart_target) {
                return false;
            }
        }
    }
    if (error) {
        return false;
    }

    std::uintmax_t destination_entries = 0U;
    for (std::filesystem::recursive_directory_iterator iterator(destination, error), end;
         !error && iterator != end;
         iterator.increment(error)) {
        if (!checked_add_uintmax(1U, destination_entries)) {
            error = std::make_error_code(std::errc::value_too_large);
            return false;
        }
    }
    return !error && source_entries == destination_entries;
}

OperationResult preflight_parent(const std::filesystem::path &source,
                                 const std::filesystem::path &destination_parent)
{
    if (source.empty() || source.filename().empty() || destination_parent.empty()) {
        return failure(OperationStatus::InvalidRequest,
                       OperationPhase::Preflight,
                       {},
                       "The source or destination is not available.");
    }

    std::error_code error;
    if (!path_present(source, error)) {
        return failure(error ? OperationEngine::status_for_error(error)
                             : OperationStatus::InvalidRequest,
                       OperationPhase::Preflight,
                       {},
                       "The selected item no longer exists.");
    }

    const auto parent_status = std::filesystem::status(destination_parent, error);
    if (error) {
        return failure(OperationEngine::status_for_error(error),
                       OperationPhase::Preflight,
                       {},
                       "The destination folder could not be inspected.");
    }
    if (!std::filesystem::is_directory(parent_status)) {
        return failure(OperationStatus::InvalidRequest,
                       OperationPhase::Preflight,
                       {},
                       "The destination is not a folder.");
    }

    return OperationResult{OperationStatus::Success,
                           OperationPhase::Preflight,
                           {},
                           {},
                           false,
                           {}};
}

bool scan_totals(const std::filesystem::path &source,
                 std::uintmax_t &bytes,
                 std::uintmax_t &items,
                 std::error_code &error, TransferControl *control)
{
    bytes = 0;
    items = 0;
    error.clear();
    if (control != nullptr && control->cancelled()) {
        error = std::make_error_code(std::errc::operation_canceled);
        return false;
    }
    const auto status = std::filesystem::symlink_status(source, error);
    if (error) {
        return false;
    }

    if (!checked_add_uintmax(1U, items)) {
        error = std::make_error_code(std::errc::value_too_large);
        return false;
    }
    if (std::filesystem::is_regular_file(status)) {
        bytes = std::filesystem::file_size(source, error);
        return !error;
    }
    if (std::filesystem::is_symlink(status)) {
        return true;
    }
    if (!std::filesystem::is_directory(status)) {
        error = std::make_error_code(std::errc::operation_not_supported);
        return false;
    }

    for (std::filesystem::recursive_directory_iterator iterator(source, error), end;
         !error && iterator != end;
         iterator.increment(error)) {
        if (control != nullptr && control->cancelled()) {
            error = std::make_error_code(std::errc::operation_canceled);
            return false;
        }
        if (!checked_add_uintmax(1U, items)) {
            error = std::make_error_code(std::errc::value_too_large);
            return false;
        }
        const auto entry_status = iterator->symlink_status(error);
        if (error) {
            return false;
        }
        if (std::filesystem::is_regular_file(entry_status)) {
            const std::uintmax_t size = iterator->file_size(error);
            if (error) {
                return false;
            }
            if (!checked_add_uintmax(size, bytes)) {
                error = std::make_error_code(std::errc::value_too_large);
                return false;
            }
        }
    }
    return !error;
}

OperationResult cancelled_result(
    const std::filesystem::path &destination,
    const bool changed = false,
    std::vector<std::filesystem::path> retained_paths = {})
{
    const std::string message =
        "Operation cancelled." + retained_paths_message(retained_paths);
    return failure(OperationStatus::Cancelled,
                   OperationPhase::Execute,
                   destination,
                   message,
                   changed,
                   std::move(retained_paths));
}

bool copy_regular_file_to_descriptor(const std::filesystem::path &source,
                                     const int output,
                                     TransferControl *control,
                                     std::error_code &error)
{
    error.clear();
    std::ifstream input(source, std::ios::binary);
    if (!input) {
        error = std::make_error_code(std::errc::io_error);
        (void)::close(output);
        return false;
    }

    std::array<char, 1024 * 1024> buffer{};
    while (input) {
        if (control != nullptr && control->cancelled()) {
            (void)::close(output);
            return false;
        }
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            std::size_t written = 0;
            const std::size_t total = static_cast<std::size_t>(count);
            while (written < total) {
                const ssize_t step = ::write(output, buffer.data() + written, total - written);
                if (step <= 0) {
                    if (step < 0 && errno == EINTR) {
                        continue;
                    }
                    error = step < 0 ? std::error_code(errno, std::generic_category())
                                     : std::make_error_code(std::errc::io_error);
                    (void)::close(output);
                    return false;
                }
                written += static_cast<std::size_t>(step);
            }
            if (control != nullptr) {
                control->add_bytes(static_cast<std::uintmax_t>(count));
            }
        }
    }
    if (!input.eof()) {
        error = std::make_error_code(std::errc::io_error);
        (void)::close(output);
        return false;
    }
    if (::close(output) != 0) {
        error = std::error_code(errno, std::generic_category());
        return false;
    }
    if (control != nullptr) {
        control->add_item();
    }
    return true;
}

bool copy_regular_file(const std::filesystem::path &source,
                       const std::filesystem::path &destination,
                       TransferControl *control,
                       destination_ownership::OwnedOutputs &owned,
                       std::error_code &error)
{
    const int output = destination_ownership::open_exclusive(destination, error);
    if (output < 0) {
        return false;
    }
    if (!destination_ownership::record_fd(destination, output, owned, error)) {
        const std::error_code record_error = error;
        (void)::close(output);
        error = record_error;
        return false;
    }
    return copy_regular_file_to_descriptor(source, output, control, error);
}

OperationResult metadata_failure(const std::filesystem::path &destination,
                                 destination_ownership::OwnedOutputs &owned,
                                 const std::error_code &error,
                                 const bool unpublished,
                                 destination_ownership::ProtectedStaging &staging,
                                 const char *reason = "The transfer could not preserve and flush file metadata: ")
{
    std::error_code cleanup_error;
    std::vector<std::filesystem::path> retained_paths;
    const bool cleaned = unpublished
                             ? destination_ownership::cleanup_unpublished(
                                   staging, destination, owned, cleanup_error, &retained_paths)
                             : destination_ownership::cleanup_owned(
                                   owned, cleanup_error, &retained_paths);
    const std::string failure_message =
        std::string(reason) + (error ? error.message() : "copied file bytes differ") +
        (cleaned ? " The owned partial destination was removed."
                 : retained_paths_message(retained_paths));
    return failure(OperationStatus::VerificationFailure,
                   OperationPhase::Verify,
                   destination,
                   failure_message,
                   !cleaned,
                   std::move(retained_paths));
}

std::string unsupported_capability_message(const bool cleaned = true)
{
    return "This location does not provide the atomic no-replace operation required for this transfer. The source and visible destination were left unchanged." +
           std::string{cleaned ? " No private staging paths remain."
                               : " A private staging path was retained for safety."};
}

OperationResult copy_exact(const std::filesystem::path &source,
                           const std::filesystem::path &destination,
                           TransferControl *control,
                           const TransferOperationHooks &hooks,
                           destination_ownership::OwnedOutputs *completed_outputs = nullptr,
                           FileContentEvidence *completed_evidence = nullptr)
{
    std::error_code error;
    if (control != nullptr && !control->has_batch_totals()) {
        std::uintmax_t bytes = 0;
        std::uintmax_t items = 0;
        if (!scan_totals(source, bytes, items, error, control)) {
            if (error == std::errc::operation_canceled) return cancelled_result(destination);
            return failure(OperationEngine::status_for_error(error),
                           OperationPhase::Preflight,
                           destination,
                           error == std::errc::value_too_large
                               ? "The selected item is too large to represent safely."
                               : "The selected item could not be measured safely.");
        }
        control->set_total(bytes, items);
    }
    if (control != nullptr && control->cancelled()) {
        return cancelled_result(destination);
    }

    const auto source_status = std::filesystem::symlink_status(source, error);
    if (error) {
        return failure(OperationEngine::status_for_error(error),
                       OperationPhase::Preflight,
                       destination,
                       "The selected item could not be inspected.");
    }

    std::vector<std::pair<std::filesystem::path, destination_ownership::OwnedOutput>> file_metadata;
    std::vector<std::pair<std::filesystem::path, destination_ownership::OwnedOutput>> directory_metadata;
    std::vector<std::pair<std::filesystem::path, destination_ownership::OwnedOutput>> link_metadata;
    destination_ownership::OwnedOutputs owned;
    destination_ownership::ProtectedStaging private_staging;
    bool unpublished = true;

    if (std::filesystem::is_symlink(source_status)) {
        const auto target = std::filesystem::read_symlink(source, error);
        std::filesystem::path private_link;
        if (!error) {
            private_link = destination_ownership::create_private_symlink(
                destination, target, private_staging, owned, error);
        }
        if (!error && !private_link.empty()) link_metadata.emplace_back(source, owned.back());
        if (!error && control != nullptr) {
            control->add_item();
        }
    } else if (std::filesystem::is_regular_file(source_status)) {
        std::filesystem::path private_file;
        const int output = destination_ownership::create_private_file(
            destination, private_staging, owned, private_file, error);
        if (output >= 0 &&
            copy_regular_file_to_descriptor(source, output, control, error)) {
            file_metadata.emplace_back(source, owned.back());
        } else if (control != nullptr && control->cancelled()) {
            std::error_code cleanup_error;
            std::vector<std::filesystem::path> retained_paths;
            const bool cleaned = destination_ownership::cleanup_unpublished(
                private_staging, destination, owned, cleanup_error, &retained_paths);
            return cancelled_result(destination, !cleaned, std::move(retained_paths));
        }
    } else if (std::filesystem::is_directory(source_status)) {
        const std::filesystem::path private_destination =
            destination_ownership::create_private_directory(
                destination, private_staging, owned, error);
        if (private_destination.empty()) {
            std::error_code cleanup_error;
            std::vector<std::filesystem::path> retained_paths;
            const bool cleaned = destination_ownership::cleanup_unpublished(
                private_staging, destination, owned, cleanup_error, &retained_paths);
            const std::string failure_message =
                "A private directory copy could not be created: " + error.message() +
                (cleaned ? std::string{} : retained_paths_message(retained_paths));
            return failure(OperationEngine::status_for_error(error),
                           OperationPhase::Execute,
                           destination,
                           failure_message,
                           !cleaned,
                           std::move(retained_paths));
        }
        directory_metadata.emplace_back(source, owned.back());
        if (!error && control != nullptr) {
            control->add_item();
        }
        for (std::filesystem::recursive_directory_iterator iterator(source, error), end;
             !error && iterator != end;
             iterator.increment(error)) {
            if (control != nullptr && control->cancelled()) {
                std::error_code cleanup_error;
                std::vector<std::filesystem::path> retained_paths;
                const bool cleaned = destination_ownership::cleanup_unpublished(
                    private_staging, destination, owned, cleanup_error, &retained_paths);
                return cancelled_result(destination, !cleaned, std::move(retained_paths));
            }

            const auto relative = iterator->path().lexically_relative(source);
            const auto target = private_destination / relative;
            const auto entry_status = iterator->symlink_status(error);
            if (error) {
                break;
            }
            if (std::filesystem::is_directory(entry_status)) {
                (void)destination_ownership::create_directory_exclusive(target, error);
                if (!error) {
                    (void)destination_ownership::record(target, owned, error);
                }
                if (!error) {
                    directory_metadata.emplace_back(iterator->path(), owned.back());
                }
                if (!error && control != nullptr) {
                    control->add_item();
                }
            } else if (std::filesystem::is_symlink(entry_status)) {
                const auto link_target = std::filesystem::read_symlink(iterator->path(), error);
                if (!error) {
                    std::filesystem::create_symlink(link_target, target, error);
                }
                if (!error) {
                    (void)destination_ownership::record(target, owned, error);
                    if (!error) link_metadata.emplace_back(iterator->path(), owned.back());
                }
                if (!error && control != nullptr) {
                    control->add_item();
                }
            } else if (std::filesystem::is_regular_file(entry_status)) {
                if (!copy_regular_file(iterator->path(), target, control, owned, error)) {
                    if (control != nullptr && control->cancelled()) {
                        std::error_code cleanup_error;
                        std::vector<std::filesystem::path> retained_paths;
                        const bool cleaned = destination_ownership::cleanup_unpublished(
                            private_staging,
                            destination,
                            owned,
                            cleanup_error,
                            &retained_paths);
                        return cancelled_result(
                            destination, !cleaned, std::move(retained_paths));
                    }
                } else {
                    file_metadata.emplace_back(iterator->path(), owned.back());
                }
            } else {
                error = std::make_error_code(std::errc::operation_not_supported);
            }
        }
    } else {
        return failure(OperationStatus::InvalidRequest,
                       OperationPhase::Preflight,
                       destination,
                       "This item type cannot be transferred yet.");
    }

    if (error) {
        const auto status = OperationEngine::status_for_error(error);
        std::error_code cleanup_error;
        std::vector<std::filesystem::path> retained_paths;
        const bool cleaned = unpublished
                                 ? destination_ownership::cleanup_unpublished(
                                       private_staging,
                                       destination,
                                       owned,
                                       cleanup_error,
                                       &retained_paths)
                                 : destination_ownership::cleanup_owned(
                                       owned, cleanup_error, &retained_paths);
        const std::string failure_message =
            status == OperationStatus::UnsupportedCapability
                ? unsupported_capability_message(cleaned) +
                      retained_paths_message(retained_paths)
                : "The transfer failed: " + error.message() +
                      (cleaned ? std::string{}
                               : retained_paths_message(retained_paths));
        return failure(status,
                       OperationPhase::Execute,
                       destination,
                       failure_message,
                       !cleaned,
                       std::move(retained_paths));
    }

    if (control != nullptr && control->cancelled()) {
        std::error_code cleanup_error;
        std::vector<std::filesystem::path> retained_paths;
        const bool cleaned = unpublished
                                 ? destination_ownership::cleanup_unpublished(
                                       private_staging,
                                       destination,
                                       owned,
                                       cleanup_error,
                                       &retained_paths)
                                 : destination_ownership::cleanup_owned(
                                       owned, cleanup_error, &retained_paths);
        return cancelled_result(destination, !cleaned, std::move(retained_paths));
    }

    // Verify byte-for-byte while the unpublished private files still have
    // readable owner permissions. Applying source modes before this check can
    // make a correctly copied group-readable file unreadable to its new owner.
    FileContentEvidence content_evidence;
    for (const auto &entry : file_metadata) {
        if (!identical_file_bytes(entry.first, entry.second.path, error)) {
            return metadata_failure(destination, owned, error, unpublished,
                                    private_staging,
                                    "The transfer could not verify copied file contents: ");
        }
        if (completed_evidence != nullptr) {
            const std::string digest = digest_file_bytes(entry.second.path, error);
            if (error) {
                return metadata_failure(destination, owned, error, unpublished,
                                        private_staging,
                                        "The transfer could not hash its private copy: ");
            }
            content_evidence.emplace_back(
                entry.first.lexically_relative(source), digest);
        }
    }

    for (const auto &entry : link_metadata) {
        if (!preserve_copied_metadata(entry.first, entry.second, error))
            return metadata_failure(destination, owned, error, unpublished, private_staging);
    }
    for (const auto &entry : file_metadata) {
        if (!preserve_copied_metadata(entry.first, entry.second, error)) {
            return metadata_failure(destination, owned, error, unpublished, private_staging);
        }
    }
    for (auto iterator = directory_metadata.rbegin(); iterator != directory_metadata.rend(); ++iterator) {
        if (!preserve_copied_metadata(iterator->first, iterator->second, error)) {
            return metadata_failure(destination, owned, error, unpublished, private_staging);
        }
    }

    {
        const std::filesystem::path private_destination = owned.front().path;
        // Payload data/metadata and private directory entries precede public
        // namespace publication. Source removal is never allowed to overtake
        // the destination's file and parent-directory durability barriers.
        if (!sync_copied_descriptor(private_staging.descriptor, error))
            return metadata_failure(destination, owned, error, unpublished, private_staging);
        if (control != nullptr && control->cancelled()) {
            std::error_code cleanup_error;
            std::vector<std::filesystem::path> retained_paths;
            const bool cleaned = destination_ownership::cleanup_unpublished(
                private_staging, destination, owned, cleanup_error, &retained_paths);
            return cancelled_result(destination, !cleaned, std::move(retained_paths));
        }
        const bool published = std::filesystem::is_symlink(source_status)
            ? hooks.publish_output(private_staging, private_destination, destination, owned.front(), error)
            : hooks.publish_tree(private_staging, private_destination, destination, owned, error);
        if (!published) {
            std::error_code cleanup_error;
            std::vector<std::filesystem::path> retained_paths;
            const auto status = error == std::errc::file_exists
                                    ? OperationStatus::DestinationConflict
                                    : OperationEngine::status_for_error(error);
            const bool cleaned = destination_ownership::cleanup_unpublished(
                private_staging, destination, owned, cleanup_error, &retained_paths);
            const std::string failure_message =
                status == OperationStatus::UnsupportedCapability
                    ? unsupported_capability_message(cleaned) +
                          retained_paths_message(retained_paths)
                    : status == OperationStatus::DestinationConflict
                          ? "The destination was claimed by another writer before publication." +
                                std::string{cleaned
                                                ? " The private copy was removed."
                                                : retained_paths_message(retained_paths)}
                          : "The private copy could not be published: " +
                                error.message() +
                                (cleaned
                                     ? " The private copy was removed."
                                     : retained_paths_message(retained_paths));
            return failure(status,
                           OperationPhase::Execute,
                           destination,
                           failure_message,
                           !cleaned,
                           std::move(retained_paths));
        }
        unpublished = false;
    }
    if (!sync_copy_publication(destination.parent_path(), error)) {
        return failure(OperationStatus::VerificationFailure, OperationPhase::Verify,
                       destination, "The copied destination could not be made durable: " +
                           error.message() + " The source was left intact. Inspect the retained copy at “" +
                           destination.string() + "”.", true, {destination});
    }

    if (!destination_ownership::verify_all(owned, error)) {
        std::error_code cleanup_error;
        std::vector<std::filesystem::path> retained_paths;
        const bool cleaned =
            destination_ownership::cleanup_owned(owned, cleanup_error, &retained_paths);
        const std::string failure_message =
            std::string{"The transfer completed but the destination identity could not be verified."} +
            (cleaned ? " The owned partial destination was removed."
                     : retained_paths_message(retained_paths));
        return failure(OperationStatus::VerificationFailure,
                       OperationPhase::Verify,
                       destination,
                       failure_message,
                       !cleaned,
                       std::move(retained_paths));
    }

    if (!equivalent_copy_shape(source, destination, error)) {
        std::error_code cleanup_error;
        std::vector<std::filesystem::path> retained_paths;
        const bool cleaned =
            destination_ownership::cleanup_owned(owned, cleanup_error, &retained_paths);
        const std::string failure_message =
            "The source changed while it was being copied; the copied snapshot was not accepted." +
            std::string{cleaned ? " The owned partial destination was removed."
                                : retained_paths_message(retained_paths)};
        return failure(OperationStatus::VerificationFailure,
                       OperationPhase::Verify,
                       destination,
                       failure_message,
                       !cleaned,
                       std::move(retained_paths));
    }

    if (completed_evidence != nullptr) {
        if (!matches_content_evidence(source, content_evidence, error)) {
            std::error_code cleanup_error;
            std::vector<std::filesystem::path> retained_paths;
            const bool cleaned = destination_ownership::cleanup_owned(
                owned, cleanup_error, &retained_paths);
            return failure(OperationStatus::VerificationFailure,
                           OperationPhase::Verify, destination,
                           "The source contents changed before the copy committed." +
                               std::string{cleaned ? " The copied destination was removed."
                                                   : retained_paths_message(retained_paths)},
                           !cleaned, std::move(retained_paths));
        }
        *completed_evidence = std::move(content_evidence);
    }
    if (completed_outputs != nullptr) {
        *completed_outputs = owned;
    }

    return OperationResult{OperationStatus::Success,
                           OperationPhase::Complete,
                           destination,
                           "Transferred " + quoted_name(destination) + ".",
                           true,
                           {}};
}

struct DestinationDecision {
    std::filesystem::path destination;
    bool skip{false};
    OperationResult failure_result{};
};

DestinationDecision choose_destination(const std::filesystem::path &source,
                                       const std::filesystem::path &destination_parent,
                                       const ConflictPolicy policy)
{
    DestinationDecision decision;
    decision.destination = destination_parent / source.filename();

    std::error_code error;
    const bool exists = path_present(decision.destination, error);
    if (error) {
        decision.failure_result = failure(OperationEngine::status_for_error(error),
                                          OperationPhase::Preflight,
                                          decision.destination,
                                          "The destination could not be checked.");
        return decision;
    }
    if (!exists) {
        return decision;
    }

    if (policy == ConflictPolicy::Skip) {
        decision.skip = true;
        return decision;
    }
    if (policy == ConflictPolicy::KeepBoth) {
        decision.destination = TransferOperationEngine::keep_both_destination(
            source, destination_parent, error);
        if (error || decision.destination.empty()) {
            decision.failure_result = failure(error ? OperationEngine::status_for_error(error)
                                                    : OperationStatus::ExecutionFailure,
                                              OperationPhase::Preflight,
                                              {},
                                              "A non-conflicting destination name could not be created.");
        }
        return decision;
    }

    decision.failure_result = failure(OperationStatus::DestinationConflict,
                                      OperationPhase::Preflight,
                                      decision.destination,
                                      "An item named " + quoted_name(decision.destination) +
                                          " already exists there.");
    return decision;
}

} // namespace

TransferProgress TransferControl::progress() const noexcept
{
    std::lock_guard<std::mutex> lock(progress_mutex_);
    return progress_;
}

bool TransferControl::has_batch_totals() const noexcept
{
    std::lock_guard<std::mutex> lock(progress_mutex_);
    return batch_mode_;
}

void TransferControl::begin_batch(const std::uintmax_t bytes, const std::uintmax_t items) noexcept
{
    std::lock_guard<std::mutex> lock(progress_mutex_);
    progress_ = TransferProgress{0, bytes, 0, items};
    batch_mode_ = true;
}

void TransferControl::set_total(const std::uintmax_t bytes, const std::uintmax_t items) noexcept
{
    std::lock_guard<std::mutex> lock(progress_mutex_);
    if (!batch_mode_) {
        progress_.bytes_total = bytes;
        progress_.items_total = items;
    }
}

void TransferControl::add_bytes(const std::uintmax_t bytes) noexcept
{
    TransferProgress snapshot;
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress_.bytes_done += bytes;
        snapshot = progress_;
    }
    if (observer_ != nullptr) {
        observer_(*this, snapshot);
    }
}

void TransferControl::add_item() noexcept
{
    TransferProgress snapshot;
    {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        ++progress_.items_done;
        snapshot = progress_;
    }
    if (observer_ != nullptr) {
        observer_(*this, snapshot);
    }
}

std::filesystem::path TransferOperationEngine::keep_both_destination(
    const std::filesystem::path &source,
    const std::filesystem::path &destination_parent,
    std::error_code &error)
{
    error.clear();
    const std::string filename = source.filename().string();
    const auto source_status = std::filesystem::symlink_status(source, error);
    if (error) {
        return {};
    }

    std::string stem = filename;
    std::string extension;
    if (!std::filesystem::is_directory(source_status)) {
        extension = source.extension().string();
        stem = source.stem().string();
        if (stem.empty()) {
            stem = filename;
            extension.clear();
        }
    }

    for (std::uintmax_t index = 1; index < 1000000; ++index) {
        const std::string suffix = index == 1
                                       ? " (copy)"
                                       : " (copy " + std::to_string(index) + ")";
        const auto candidate = destination_parent / (stem + suffix + extension);
        if (!path_present(candidate, error)) {
            return error ? std::filesystem::path{} : candidate;
        }
        if (error) {
            return {};
        }
    }
    error = std::make_error_code(std::errc::filename_too_long);
    return {};
}

OperationResult TransferOperationEngine::copy_item(const std::filesystem::path &source,
                                                    const std::filesystem::path &destination_parent,
                                                    const ConflictPolicy policy,
                                                    TransferControl *control) const
{
    if (control != nullptr && control->cancelled()) return cancelled_result({});
    OperationResult preflight = preflight_parent(source, destination_parent);
    if (!preflight.ok()) {
        return preflight;
    }

    std::error_code error;
    const auto source_status = std::filesystem::symlink_status(source, error);
    if (error) {
        return failure(OperationEngine::status_for_error(error),
                       OperationPhase::Preflight,
                       {},
                       "The selected item could not be inspected.");
    }
    if (std::filesystem::is_directory(source_status) &&
        path_is_within(destination_parent, source, error)) {
        return failure(OperationStatus::InvalidRequest,
                       OperationPhase::Preflight,
                       {},
                       "A folder cannot be copied into itself or one of its descendants.");
    }
    if (error) {
        return failure(OperationEngine::status_for_error(error),
                       OperationPhase::Preflight,
                       {},
                       "The source and destination could not be resolved safely.");
    }

    DestinationDecision decision = choose_destination(source, destination_parent, policy);
    if (!decision.failure_result.message.empty()) {
        return decision.failure_result;
    }
    if (decision.skip) {
        return OperationResult{OperationStatus::Success,
                               OperationPhase::Complete,
                               decision.destination,
                               "Skipped " + quoted_name(source) + ".",
                               false,
                               {}};
    }

    OperationResult result = copy_exact(source, decision.destination, control, hooks_);
    if (result.ok()) {
        result.message = "Copied " + quoted_name(source) +
                         (decision.destination.filename() == source.filename()
                              ? "."
                              : " as " + quoted_name(decision.destination) + ".");
    }
    return result;
}

OperationResult TransferOperationEngine::move_item(const std::filesystem::path &source,
                                                    const std::filesystem::path &destination_parent,
                                                    const ConflictPolicy policy,
                                                    TransferControl *control) const
{
    if (control != nullptr && control->cancelled()) return cancelled_result({});
    OperationResult preflight = preflight_parent(source, destination_parent);
    if (!preflight.ok()) {
        return preflight;
    }
    if (source.parent_path() == destination_parent) {
        return OperationResult{OperationStatus::Success,
                               OperationPhase::Complete,
                               source,
                               "The item is already in that folder.",
                               false,
                               {}};
    }

    std::error_code error;
    const auto source_status = std::filesystem::symlink_status(source, error);
    if (error) {
        return failure(OperationEngine::status_for_error(error),
                       OperationPhase::Preflight,
                       {},
                       "The selected item could not be inspected.");
    }
    if (std::filesystem::is_directory(source_status) &&
        path_is_within(destination_parent, source, error)) {
        return failure(OperationStatus::InvalidRequest,
                       OperationPhase::Preflight,
                       {},
                       "A folder cannot be moved into itself or one of its descendants.");
    }
    if (error) {
        return failure(OperationEngine::status_for_error(error),
                       OperationPhase::Preflight,
                       {},
                       "The source and destination could not be resolved safely.");
    }

    DestinationDecision decision = choose_destination(source, destination_parent, policy);
    if (!decision.failure_result.message.empty()) {
        return decision.failure_result;
    }
    if (decision.skip) {
        return OperationResult{OperationStatus::Success,
                               OperationPhase::Complete,
                               decision.destination,
                               "Skipped " + quoted_name(source) + ".",
                               false,
                               {}};
    }

    std::uintmax_t bytes = 0;
    std::uintmax_t items = 0;
    if (control != nullptr) {
        if (!scan_totals(source, bytes, items, error, control)) {
            if (error == std::errc::operation_canceled) return cancelled_result(decision.destination);
            return failure(OperationEngine::status_for_error(error), OperationPhase::Preflight,
                           decision.destination, "The selected item could not be measured safely.");
        }
        control->set_total(bytes, items);
        if (control->cancelled()) return cancelled_result(decision.destination);
    }
    error.clear();
    const auto moved_identity = destination_ownership::identity_for(source, error);
    if (error) {
        return failure(OperationEngine::status_for_error(error),
                       OperationPhase::Preflight,
                       decision.destination,
                       "The selected item identity could not be inspected.");
    }

    const destination_ownership::OwnedOutput source_output{source, moved_identity};
    destination_ownership::OwnedOutput moved_output;
    std::filesystem::path retained_source_quarantine;
    if (hooks_.move_owned(
            source_output,
            decision.destination,
            moved_output,
            error,
            &retained_source_quarantine)) {
        std::error_code verify_error;
        if (!destination_ownership::same_object(
                decision.destination, moved_output.identity, verify_error)) {
            return failure(OperationStatus::VerificationFailure,
                           OperationPhase::Verify,
                           decision.destination,
                           "The move completed but the destination identity changed before it could be verified.",
                           true);
        }
        if (control != nullptr) {
            control->add_bytes(bytes);
            for (std::uintmax_t index = 0; index < items; ++index) {
                control->add_item();
            }
        }
        return OperationResult{OperationStatus::Success,
                               OperationPhase::Complete,
                               decision.destination,
                               "Moved " + quoted_name(decision.destination) + ".",
                               true,
                               {}};
    }

    if (error != std::errc::cross_device_link) {
        const auto status = OperationEngine::status_for_error(error);
        std::vector<std::filesystem::path> retained_paths;
        if (!retained_source_quarantine.empty()) {
            retained_paths.push_back(retained_source_quarantine);
        }
        const std::string failure_message =
            status == OperationStatus::UnsupportedCapability
                ? unsupported_capability_message() +
                      retained_paths_message(retained_paths)
                : "The item could not be moved: " + error.message() +
                      retained_paths_message(retained_paths);
        const bool changed = !retained_paths.empty();
        return failure(status,
                       OperationPhase::Execute,
                       decision.destination,
                       failure_message,
                       changed,
                       std::move(retained_paths));
    }

    destination_ownership::OwnedOutputs copied_outputs;
    FileContentEvidence content_evidence;
    OperationResult copied =
        copy_exact(source, decision.destination, control, hooks_,
                   &copied_outputs, &content_evidence);
    if (!copied.ok()) {
        return copied;
    }

    // A fully verified copy is the cross-volume move commit point. A cancellation
    // observed after that point must not be reported as if no work completed.
    if (!destination_ownership::verify_all(copied_outputs, error)) {
        std::error_code cleanup_error;
        std::vector<std::filesystem::path> retained_paths;
        const bool cleaned = destination_ownership::cleanup_owned(
            copied_outputs, cleanup_error, &retained_paths);
        const std::string failure_message =
            "The copied destination changed before the move could commit." +
            std::string{cleaned ? " The owned copy was removed."
                                : retained_paths_message(retained_paths)};
        return failure(OperationStatus::VerificationFailure,
                       OperationPhase::Verify,
                       decision.destination,
                       failure_message,
                       !cleaned,
                       std::move(retained_paths));
    }

    destination_ownership::ProtectedStaging source_staging;
    std::filesystem::path source_quarantine;
    retained_source_quarantine.clear();
    if (!hooks_.quarantine_owned(source_output,
                                 source_staging,
                                 source_quarantine,
                                 error,
                                 &retained_source_quarantine)) {
        const auto status = OperationEngine::status_for_error(error);
        std::error_code cleanup_error;
        std::vector<std::filesystem::path> retained_paths;
        const bool cleaned = destination_ownership::cleanup_owned(
            copied_outputs, cleanup_error, &retained_paths);
        const std::string destination_cleanup_message =
            cleaned ? " The copied destination was removed."
                    : retained_paths_message(retained_paths);
        std::string retained_source_message;
        if (!retained_source_quarantine.empty()) {
            retained_paths.push_back(retained_source_quarantine);
            retained_source_message =
                " The source data was retained at “" +
                retained_source_quarantine.string() + "”.";
        }
        return failure(status,
                       status == OperationStatus::UnsupportedCapability
                           ? OperationPhase::Execute
                           : OperationPhase::Verify,
                       decision.destination,
                       status == OperationStatus::UnsupportedCapability
                           ? "The source filesystem cannot provide the atomic quarantine required to commit this cross-volume move. The source was left in place." +
                                 destination_cleanup_message +
                                 retained_source_message
                           : "The source changed before the cross-volume move could commit." +
                                 destination_cleanup_message +
                                 retained_source_message,
                       !cleaned || !retained_source_quarantine.empty(),
                       std::move(retained_paths));
    }

    if (!destination_ownership::verify_all(copied_outputs, error)) {
        std::error_code restore_error;
        bool source_absent = false;
        const bool restored = destination_ownership::restore_quarantine(
            source_staging,
            source,
            restore_error,
            &retained_source_quarantine,
            &source_absent);
        std::error_code cleanup_error;
        std::vector<std::filesystem::path> retained_paths;
        const bool cleaned = destination_ownership::cleanup_owned(
            copied_outputs, cleanup_error, &retained_paths);
        if (!restored && !source_absent) {
            retained_paths.push_back(retained_source_quarantine.empty()
                                         ? source_quarantine
                                         : retained_source_quarantine);
        }
        const std::string failure_message =
            "The copied destination changed while the source was being committed." +
            std::string{cleaned ? " The copied destination was removed."
                                : retained_paths_message(retained_paths)} +
            (restored ? " The source was restored to its original name."
                      : retained_paths_message(retained_paths));
        return failure(OperationStatus::VerificationFailure,
                       OperationPhase::Verify,
                       decision.destination,
                       failure_message,
                       !cleaned || !restored,
                       std::move(retained_paths));
    }

    if (!equivalent_copy_shape(source_quarantine, decision.destination, error) ||
        !matches_content_evidence(source_quarantine, content_evidence, error)) {
        std::error_code restore_error;
        bool source_absent = false;
        const bool restored = destination_ownership::restore_quarantine(
            source_staging,
            source,
            restore_error,
            &retained_source_quarantine,
            &source_absent);
        std::error_code cleanup_error;
        std::vector<std::filesystem::path> retained_paths;
        const bool cleaned = destination_ownership::cleanup_owned(
            copied_outputs, cleanup_error, &retained_paths);
        if (!restored && !source_absent) {
            retained_paths.push_back(retained_source_quarantine.empty()
                                         ? source_quarantine
                                         : retained_source_quarantine);
        }
        const std::string failure_message =
            "The source changed while it was being copied; the move was not committed." +
            std::string{cleaned ? " The owned copy was removed."
                                : retained_paths_message(retained_paths)} +
            (restored ? " The source was restored to its original name."
                      : retained_paths_message(retained_paths));
        return failure(OperationStatus::VerificationFailure,
                       OperationPhase::Verify,
                       decision.destination,
                       failure_message,
                       !cleaned || !restored,
                       std::move(retained_paths));
    }

    hooks_.remove_source_quarantine(source_quarantine, error);
    if (error) {
        const std::error_code remove_error = error;
        std::error_code restore_error;
        const bool remains =
            hooks_.source_quarantine_present(source_quarantine, restore_error);
        bool restored = false;
        retained_source_quarantine.clear();
        if (!restore_error && remains) {
            restored = destination_ownership::restore_quarantine(
                source_staging, source, restore_error, &retained_source_quarantine);
        }
        std::vector<std::filesystem::path> retained_paths;
        if (restore_error) {
            if (!retained_source_quarantine.empty()) {
                retained_paths.push_back(retained_source_quarantine);
            } else {
                std::error_code retain_error;
                if (hooks_.retain_source_quarantine(
                        source_staging,
                        source,
                        retained_source_quarantine,
                        retain_error)) {
                    retained_paths.push_back(retained_source_quarantine);
                } else {
                    bool source_absent = false;
                    std::error_code publication_error;
                    if (destination_ownership::publish_staged_payload_evidence(
                            source_staging,
                            source,
                            retained_source_quarantine,
                            source_absent,
                            publication_error) &&
                        !source_absent) {
                        retained_paths.push_back(retained_source_quarantine);
                    }
                }
            }
        }
        const std::string source_recovery_message =
            restore_error
                ? retained_paths_message(retained_paths) + " " +
                      restore_error.message()
                : (restored
                       ? " Remaining source data was restored to its original name."
                       : " No remaining source object required restoration.");
        return failure(OperationEngine::status_for_error(remove_error),
                       OperationPhase::Execute,
                       decision.destination,
                       "The item was copied, but the original could not be removed: " +
                           remove_error.message() +
                           source_recovery_message,
                       true,
                       std::move(retained_paths));
    }

    destination_ownership::release_staging(source_staging);
    if (!sync_copy_publication(source.parent_path(), error)) {
        return failure(OperationStatus::VerificationFailure, OperationPhase::Verify,
                       decision.destination,
                       "The copied destination is durable, but source removal could not be made durable: " +
                           error.message() + " Inspect the destination at “" + decision.destination.string() + "”.",
                       true, {decision.destination});
    }

    return OperationResult{OperationStatus::Success,
                           OperationPhase::Complete,
                           decision.destination,
                           "Moved " + quoted_name(decision.destination) + ".",
                           true,
                           {}};
}

} // namespace infiltrator::files
