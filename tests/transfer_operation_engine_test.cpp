// SPDX-License-Identifier: GPL-3.0-or-later
#include "transfer_operation_engine.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>

using infiltrator::files::ConflictPolicy;
using infiltrator::files::OperationStatus;
using infiltrator::files::TransferControl;
using infiltrator::files::TransferOperationEngine;

namespace {

bool write_text(const std::filesystem::path &path, const std::string &text)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
    return stream.good();
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

    fs::remove_all(root, cleanup_error);
    return 0;
}
