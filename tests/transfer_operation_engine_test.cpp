// SPDX-License-Identifier: GPL-3.0-or-later
#include "transfer_operation_engine.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <unistd.h>

using infiltrator::files::ConflictPolicy;
using infiltrator::files::OperationStatus;
using infiltrator::files::TransferControl;
using infiltrator::files::TransferOperationEngine;
using infiltrator::files::TransferOperationHooks;

namespace {

bool write_text(const std::filesystem::path &path, const std::string &text)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
    return stream.good();
}

std::string read_text(const std::filesystem::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

bool force_cross_device(
    const infiltrator::files::destination_ownership::OwnedOutput &,
    const std::filesystem::path &,
    infiltrator::files::destination_ownership::OwnedOutput &,
    std::error_code &error,
    std::filesystem::path *retained) noexcept
{
    if (retained != nullptr) {
        retained->clear();
    }
    error = std::make_error_code(std::errc::cross_device_link);
    return false;
}

bool force_unsupported_quarantine(
    const infiltrator::files::destination_ownership::OwnedOutput &,
    infiltrator::files::destination_ownership::ProtectedStaging &,
    std::filesystem::path &quarantine,
    std::error_code &error,
    std::filesystem::path *retained) noexcept
{
    quarantine.clear();
    if (retained != nullptr) {
        retained->clear();
    }
    error = std::make_error_code(std::errc::operation_not_supported);
    return false;
}

void force_quarantine_remove_failure(
    const std::filesystem::path &, std::error_code &error) noexcept
{
    error = std::make_error_code(std::errc::io_error);
}

bool force_quarantine_presence_failure(
    const std::filesystem::path &, std::error_code &error) noexcept
{
    error = std::make_error_code(std::errc::permission_denied);
    return false;
}

unsigned int unsupported_publication_calls = 0U;

bool force_unsupported_publication(
    infiltrator::files::destination_ownership::ProtectedStaging &,
    const std::filesystem::path &,
    const std::filesystem::path &,
    infiltrator::files::destination_ownership::OwnedOutputs &,
    std::error_code &error) noexcept
{
    ++unsupported_publication_calls;
    error = std::make_error_code(std::errc::operation_not_supported);
    return false;
}

bool force_permission_denied_publication(
    infiltrator::files::destination_ownership::ProtectedStaging &,
    const std::filesystem::path &,
    const std::filesystem::path &,
    infiltrator::files::destination_ownership::OwnedOutputs &,
    std::error_code &error) noexcept
{
    error = std::make_error_code(std::errc::permission_denied);
    return false;
}

void cancel_after_private_root(TransferControl &control,
                               const infiltrator::files::TransferProgress &progress) noexcept
{
    if (progress.items_done >= 1U) {
        control.request_cancel();
    }
}

bool has_internal_residue(const std::filesystem::path &root)
{
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(root, error), end;
         !error && iterator != end;
         iterator.increment(error)) {
        if (iterator->path().filename().string().starts_with(".infiltrator-")) {
            return true;
        }
    }
    return static_cast<bool>(error);
}

} // namespace

int main()
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
                          ("infiltrator-files-transfer-test-" + std::to_string(::getpid()));
    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
    fs::create_directories(root / "destination");

    TransferOperationEngine engine;
    const fs::path source = root / "report.txt";
    if (!write_text(source, "transfer payload\n")) {
        return 1;
    }

    std::error_code metadata_error;
    const auto expected_permissions = fs::perms::owner_read | fs::perms::owner_write |
                                      fs::perms::owner_exec | fs::perms::group_read;
    fs::permissions(source, expected_permissions, fs::perm_options::replace, metadata_error);
    const auto expected_time = fs::file_time_type::clock::now() - std::chrono::hours(2);
    fs::last_write_time(source, expected_time, metadata_error);
    if (metadata_error) {
        fs::remove_all(root, cleanup_error);
        return 2;
    }

    TransferControl first_control;
    const auto first = engine.copy_item(source,
                                        root / "destination",
                                        ConflictPolicy::Fail,
                                        &first_control);
    const fs::path first_destination = root / "destination" / "report.txt";
    if (!first.ok() || !first.changed || !fs::exists(first_destination)) {
        fs::remove_all(root, cleanup_error);
        return 3;
    }
    const auto first_progress = first_control.progress();
    if (first_progress.bytes_total == 0U ||
        first_progress.bytes_done != first_progress.bytes_total ||
        first_progress.items_done != first_progress.items_total) {
        fs::remove_all(root, cleanup_error);
        return 4;
    }
    if (fs::status(first_destination, metadata_error).permissions() != expected_permissions ||
        metadata_error || fs::last_write_time(first_destination, metadata_error) != expected_time ||
        metadata_error) {
        fs::remove_all(root, cleanup_error);
        return 5;
    }

    const auto conflict = engine.copy_item(source,
                                           root / "destination",
                                           ConflictPolicy::Fail);
    if (conflict.status != OperationStatus::DestinationConflict || conflict.changed) {
        fs::remove_all(root, cleanup_error);
        return 6;
    }

    const auto skipped = engine.copy_item(source,
                                          root / "destination",
                                          ConflictPolicy::Skip);
    if (!skipped.ok() || skipped.changed) {
        fs::remove_all(root, cleanup_error);
        return 7;
    }

    const auto kept = engine.copy_item(source,
                                       root / "destination",
                                       ConflictPolicy::KeepBoth);
    if (!kept.ok() || !kept.changed ||
        kept.destination.filename() != "report (copy).txt" ||
        !fs::exists(kept.destination)) {
        fs::remove_all(root, cleanup_error);
        return 8;
    }

    const auto kept_again = engine.copy_item(source,
                                             root / "destination",
                                             ConflictPolicy::KeepBoth);
    if (!kept_again.ok() ||
        kept_again.destination.filename() != "report (copy 2).txt" ||
        !fs::exists(kept_again.destination)) {
        fs::remove_all(root, cleanup_error);
        return 9;
    }

    TransferControl cancelled_control;
    cancelled_control.request_cancel();
    fs::create_directories(root / "cancel-destination");
    const auto cancelled = engine.copy_item(source,
                                            root / "cancel-destination",
                                            ConflictPolicy::Fail,
                                            &cancelled_control);
    if (cancelled.status != OperationStatus::Cancelled || cancelled.changed ||
        fs::exists(root / "cancel-destination" / "report.txt")) {
        fs::remove_all(root, cleanup_error);
        return 10;
    }

    const fs::path tree = root / "tree";
    fs::create_directories(tree / "nested");
    if (!write_text(tree / "nested" / "data.bin", "tree payload\n")) {
        fs::remove_all(root, cleanup_error);
        return 11;
    }
    const auto nested_permissions = fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec;
    fs::permissions(tree / "nested", nested_permissions, fs::perm_options::replace, metadata_error);
    const auto nested_time = fs::file_time_type::clock::now() - std::chrono::hours(4);
    fs::last_write_time(tree / "nested", nested_time, metadata_error);
    if (metadata_error) {
        fs::remove_all(root, cleanup_error);
        return 12;
    }

    fs::create_directories(root / "tree-destination");
    TransferControl tree_control;
    const auto tree_copy = engine.copy_item(tree,
                                            root / "tree-destination",
                                            ConflictPolicy::Fail,
                                            &tree_control);
    const fs::path copied_nested = root / "tree-destination" / "tree" / "nested";
    if (!tree_copy.ok() || !fs::is_regular_file(copied_nested / "data.bin")) {
        fs::remove_all(root, cleanup_error);
        return 13;
    }
    const auto tree_progress = tree_control.progress();
    if (tree_progress.items_total < 3U || tree_progress.items_done != tree_progress.items_total) {
        fs::remove_all(root, cleanup_error);
        return 14;
    }
    if (fs::status(copied_nested, metadata_error).permissions() != nested_permissions ||
        metadata_error || fs::last_write_time(copied_nested, metadata_error) != nested_time ||
        metadata_error) {
        fs::remove_all(root, cleanup_error);
        return 15;
    }

    const fs::path link_source = root / "report-link";
    const fs::path link_destination_parent = root / "link-destination";
    fs::create_directories(link_destination_parent);
    fs::create_symlink("report.txt", link_source, metadata_error);
    if (metadata_error) {
        fs::remove_all(root, cleanup_error);
        return 16;
    }
    const auto link_copy = engine.copy_item(link_source,
                                            link_destination_parent,
                                            ConflictPolicy::Fail);
    const fs::path copied_link = link_destination_parent / link_source.filename();
    metadata_error.clear();
    const fs::path copied_target = fs::read_symlink(copied_link, metadata_error);
    if (!link_copy.ok() || metadata_error || copied_target != "report.txt") {
        fs::remove_all(root, cleanup_error);
        return 17;
    }

    const fs::path movable = root / "movable.txt";
    if (!write_text(movable, "move payload\n")) {
        fs::remove_all(root, cleanup_error);
        return 18;
    }
    fs::create_directories(root / "move-destination");
    if (!write_text(root / "move-destination" / "movable.txt", "occupied\n")) {
        fs::remove_all(root, cleanup_error);
        return 19;
    }
    const auto moved = engine.move_item(movable,
                                        root / "move-destination",
                                        ConflictPolicy::KeepBoth);
    if (!moved.ok() || fs::exists(movable) ||
        moved.destination.filename() != "movable (copy).txt" ||
        !fs::exists(moved.destination)) {
        fs::remove_all(root, cleanup_error);
        return 20;
    }

    const fs::path unsupported_source = root / "unsupported-move.txt";
    const fs::path unsupported_destination_parent = root / "unsupported-destination";
    fs::create_directories(unsupported_destination_parent);
    if (!write_text(unsupported_source, "unsupported move payload\n")) {
        fs::remove_all(root, cleanup_error);
        return 21;
    }
    TransferOperationHooks unsupported_hooks;
    unsupported_hooks.move_owned = force_cross_device;
    unsupported_hooks.quarantine_owned = force_unsupported_quarantine;
    TransferOperationEngine unsupported_engine{unsupported_hooks};
    const auto unsupported_move = unsupported_engine.move_item(
        unsupported_source, unsupported_destination_parent, ConflictPolicy::Fail);
    if (unsupported_move.status != OperationStatus::UnsupportedCapability ||
        unsupported_move.changed || !unsupported_move.retained_paths.empty() ||
        !fs::exists(unsupported_source) ||
        fs::exists(unsupported_destination_parent / unsupported_source.filename()) ||
        unsupported_move.message.find("source was left in place") == std::string::npos ||
        unsupported_move.message.find("copied destination was removed") == std::string::npos) {
        fs::remove_all(root, cleanup_error);
        return 22;
    }

    const fs::path durable_source = root / "durable-source.txt";
    const fs::path durable_destination_parent = root / "durable-destination";
    fs::create_directories(durable_destination_parent);
    if (!write_text(durable_source, "durable source payload\n")) {
        fs::remove_all(root, cleanup_error);
        return 27;
    }
    TransferOperationHooks durable_hooks;
    durable_hooks.move_owned = force_cross_device;
    durable_hooks.remove_source_quarantine = force_quarantine_remove_failure;
    durable_hooks.source_quarantine_present = force_quarantine_presence_failure;
    TransferOperationEngine durable_engine{durable_hooks};
    const auto durable_move = durable_engine.move_item(
        durable_source, durable_destination_parent, ConflictPolicy::Fail);
    if (durable_move.status != OperationStatus::ExecutionFailure ||
        !durable_move.changed || durable_move.retained_paths.size() != 1U ||
        durable_move.retained_paths.front().string().find("/proc/self/fd/") !=
            std::string::npos ||
        !fs::exists(durable_move.retained_paths.front()) ||
        read_text(durable_move.retained_paths.front()) !=
            "durable source payload\n" ||
        fs::exists(durable_source) ||
        read_text(durable_destination_parent / durable_source.filename()) !=
            "durable source payload\n" ||
        durable_move.message.find(durable_move.retained_paths.front().string()) ==
            std::string::npos) {
        fs::remove_all(root, cleanup_error);
        return 28;
    }

    const fs::path cancelled_tree = root / "cancelled-tree";
    fs::create_directories(cancelled_tree / "nested");
    if (!write_text(cancelled_tree / "nested" / "data.bin", "cancelled tree payload\n")) {
        fs::remove_all(root, cleanup_error);
        return 23;
    }
    const fs::path unsupported_private_parent = root / "unsupported-private-destination";
    fs::create_directories(unsupported_private_parent);
    unsupported_hooks.publish_tree = force_unsupported_publication;
    TransferOperationEngine unsupported_private_engine{unsupported_hooks};
    TransferControl traversal_cancel{cancel_after_private_root};
    unsupported_publication_calls = 0U;
    const auto cancelled_traversal = unsupported_private_engine.copy_item(
        cancelled_tree,
        unsupported_private_parent,
        ConflictPolicy::Fail,
        &traversal_cancel);
    if (cancelled_traversal.status != OperationStatus::Cancelled ||
        cancelled_traversal.changed || !cancelled_traversal.retained_paths.empty() ||
        unsupported_publication_calls != 0U ||
        fs::exists(unsupported_private_parent / cancelled_tree.filename()) ||
        has_internal_residue(unsupported_private_parent)) {
        fs::remove_all(root, cleanup_error);
        return 24;
    }

    const fs::path denied_source = root / "permission-publication.txt";
    const fs::path denied_parent = root / "permission-publication-destination";
    fs::create_directories(denied_parent);
    if (!write_text(denied_source, "publication error payload\n")) {
        fs::remove_all(root, cleanup_error);
        return 25;
    }
    TransferOperationHooks denied_hooks;
    denied_hooks.publish_tree = force_permission_denied_publication;
    TransferOperationEngine denied_engine{denied_hooks};
    const auto denied = denied_engine.copy_item(
        denied_source, denied_parent, ConflictPolicy::Fail);
    if (denied.status != OperationStatus::PermissionFailure || denied.changed ||
        !denied.retained_paths.empty() ||
        denied.message.find("Permission denied") == std::string::npos ||
        denied.message.find("another writer") != std::string::npos ||
        fs::exists(denied_parent / denied_source.filename()) ||
        has_internal_residue(denied_parent)) {
        fs::remove_all(root, cleanup_error);
        return 26;
    }

    fs::remove_all(root, cleanup_error);
    return 0;
}

