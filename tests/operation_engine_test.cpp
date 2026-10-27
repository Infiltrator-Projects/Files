// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_engine.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

using infiltrator::files::OperationEngine;
using infiltrator::files::OperationStatus;

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
                          ("infiltrator-files-operation-test-" + std::to_string(::getpid()));
    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
    fs::create_directories(root);

    OperationEngine engine;

    const auto create_success = engine.create_directory(root, "created");
    if (!create_success.ok() || !create_success.changed || !fs::is_directory(root / "created")) {
        fs::remove_all(root, cleanup_error);
        return 1;
    }

    const auto create_conflict = engine.create_directory(root, "created");
    if (create_conflict.status != OperationStatus::DestinationConflict || create_conflict.changed) {
        fs::remove_all(root, cleanup_error);
        return 2;
    }

    std::string reason;
    if (OperationEngine::validate_item_name("", &reason) || reason.empty() ||
        OperationEngine::validate_item_name(".") ||
        OperationEngine::validate_item_name("..") ||
        OperationEngine::validate_item_name("a/b")) {
        fs::remove_all(root, cleanup_error);
        return 3;
    }

    if (OperationEngine::status_for_error(
            std::make_error_code(std::errc::permission_denied)) !=
        OperationStatus::PermissionFailure) {
        fs::remove_all(root, cleanup_error);
        return 4;
    }

    if (OperationEngine::status_for_error(
            std::make_error_code(std::errc::read_only_file_system)) !=
        OperationStatus::ReadOnlyLocation) {
        fs::remove_all(root, cleanup_error);
        return 5;
    }

    if (OperationEngine::status_for_error(
            std::make_error_code(std::errc::operation_not_supported)) !=
        OperationStatus::UnsupportedCapability) {
        fs::remove_all(root, cleanup_error);
        return 91;
    }

    // Permission changes must be descriptor-bound, refuse symlink targets and
    // reject attempts to silently erase privileged/special mode bits.
    const fs::path permission_target = root / "mode-test.txt";
    if (!write_text(permission_target, "mode fixture") ||
        ::chmod(permission_target.c_str(), 0644) != 0) {
        fs::remove_all(root, cleanup_error);
        return 110;
    }
    const auto changed_mode = engine.change_permissions(permission_target, 0600U);
    struct stat mode_check {};
    if (!changed_mode.ok() || !changed_mode.changed ||
        ::stat(permission_target.c_str(), &mode_check) != 0 ||
        (mode_check.st_mode & 0777) != 0600) {
        fs::remove_all(root, cleanup_error);
        return 111;
    }
    const auto same_mode = engine.change_permissions(permission_target, 0600U);
    const auto invalid_mode = engine.change_permissions(permission_target, 01000U);
    const fs::path permission_link = root / "mode-link";
    fs::create_symlink(permission_target, permission_link);
    const auto link_mode = engine.change_permissions(permission_link, 0644U);
    if (!same_mode.ok() || same_mode.changed ||
        invalid_mode.status != OperationStatus::InvalidRequest || invalid_mode.changed ||
        link_mode.ok() || link_mode.changed ||
        ::stat(permission_target.c_str(), &mode_check) != 0 ||
        (mode_check.st_mode & 0777) != 0600) {
        fs::remove_all(root, cleanup_error);
        return 112;
    }

    // Owners must be able to recover their own mode-000 files without Files
    // silently relying on pathname chmod or following a substituted symlink.
    if (::chmod(permission_target.c_str(), 0000) != 0) {
        fs::remove_all(root, cleanup_error);
        return 113;
    }
    const auto restored_mode = engine.change_permissions(permission_target, 0600U);
    if (!restored_mode.ok() || !restored_mode.changed ||
        ::stat(permission_target.c_str(), &mode_check) != 0 ||
        (mode_check.st_mode & 0777) != 0600) {
        (void)::chmod(permission_target.c_str(), 0600);
        fs::remove_all(root, cleanup_error);
        return 114;
    }

    // Both link types must be atomic no-replace operations, independent of the
    // displayed item name. Symlinks retain the original relative pathname;
    // hard links retain the exact source inode.
    const auto symbolic = engine.create_symbolic_link(permission_target, "mode-symbolic");
    const auto hard = engine.create_hard_link(permission_target, "mode-hard");
    struct stat source_identity{}, linked_identity{};
    std::error_code link_error;
    if (!symbolic.ok() || !hard.ok() ||
        !fs::is_symlink(fs::symlink_status(root / "mode-symbolic", link_error)) ||
        fs::read_symlink(root / "mode-symbolic", link_error) != fs::path("mode-test.txt") ||
        ::stat(permission_target.c_str(), &source_identity) != 0 ||
        ::stat((root / "mode-hard").c_str(), &linked_identity) != 0 ||
        source_identity.st_dev != linked_identity.st_dev ||
        source_identity.st_ino != linked_identity.st_ino ||
        engine.create_symbolic_link(permission_target, "mode-symbolic").status !=
            OperationStatus::DestinationConflict ||
        engine.create_hard_link(permission_target, "mode-hard").status !=
            OperationStatus::DestinationConflict ||
        engine.create_hard_link(root, "no-directory-hardlink").ok() ||
        engine.create_symbolic_link(permission_target, "../escape").ok()) {
        fs::remove_all(root, cleanup_error);
        return 115;
    }

    // New File must be no-replace, empty, and never damage an existing item.
    const auto new_file = engine.create_empty_file(root, "blank.txt");
    if (!new_file.ok() || !new_file.changed ||
        !fs::is_regular_file(root / "blank.txt") || fs::file_size(root / "blank.txt") != 0) {
        fs::remove_all(root, cleanup_error);
        return 93;
    }
    const auto existing_file = engine.create_empty_file(root, "blank.txt");
    if (existing_file.status != OperationStatus::DestinationConflict ||
        existing_file.changed || fs::file_size(root / "blank.txt") != 0) {
        fs::remove_all(root, cleanup_error);
        return 94;
    }
    const auto bad_file = engine.create_empty_file(root, "../escape.txt");
    if (bad_file.status != OperationStatus::InvalidRequest || bad_file.changed) {
        fs::remove_all(root, cleanup_error);
        return 95;
    }

    // Sparse files occupy a large logical range without a streamed zero-fill.
    const auto sparse = engine.create_sparse_file(root, "hole.bin", 8ULL * 1024U * 1024U * 1024U);
    struct stat sparse_status {};
    if (!sparse.ok() || !sparse.changed ||
        ::stat((root / "hole.bin").c_str(), &sparse_status) != 0 ||
        static_cast<std::uint64_t>(sparse_status.st_size) != 8ULL * 1024U * 1024U * 1024U ||
        engine.create_sparse_file(root, "hole.bin", 4096U).status !=
            OperationStatus::DestinationConflict ||
        engine.create_sparse_file(root, "../escape", 4096U).ok() ||
        engine.create_sparse_file(root, "too-large", (1ULL << 40U) + 1U).ok()) {
        fs::remove_all(root, cleanup_error);
        return 116;
    }

    const fs::path original = root / "original.txt";
    if (!write_text(original, "rename-copy-move\n")) {
        fs::remove_all(root, cleanup_error);
        return 6;
    }

    const auto rename_success = engine.rename_item(original, "renamed.txt");
    const fs::path renamed = root / "renamed.txt";
    if (!rename_success.ok() || !rename_success.changed || fs::exists(original) ||
        !fs::is_regular_file(renamed)) {
        fs::remove_all(root, cleanup_error);
        return 7;
    }

    if (!write_text(root / "occupied.txt", "occupied\n")) {
        fs::remove_all(root, cleanup_error);
        return 8;
    }
    const auto rename_conflict = engine.rename_item(renamed, "occupied.txt");
    if (rename_conflict.status != OperationStatus::DestinationConflict ||
        rename_conflict.changed || !fs::exists(renamed)) {
        fs::remove_all(root, cleanup_error);
        return 9;
    }

    const fs::path previous_directory = fs::current_path(cleanup_error);
    if (cleanup_error) {
        fs::remove_all(root, cleanup_error);
        return 90;
    }
    fs::current_path(root, cleanup_error);
    if (cleanup_error || !write_text("relative-source.txt", "relative rename\n")) {
        fs::current_path(previous_directory, cleanup_error);
        fs::remove_all(root, cleanup_error);
        return 91;
    }
    const auto relative_rename = engine.rename_item("relative-source.txt", "relative-destination.txt");
    const bool relative_rename_ok =
        relative_rename.ok() && relative_rename.changed &&
        !fs::exists("relative-source.txt") && fs::is_regular_file("relative-destination.txt");
    fs::current_path(previous_directory, cleanup_error);
    if (cleanup_error || !relative_rename_ok) {
        fs::remove_all(root, cleanup_error);
        return 92;
    }

    const fs::path copy_destination = root / "copy-destination";
    fs::create_directories(copy_destination);
    const auto copy_success = engine.copy_item(renamed, copy_destination);
    const fs::path copied_file = copy_destination / renamed.filename();
    if (!copy_success.ok() || !copy_success.changed || !fs::exists(renamed) ||
        !fs::is_regular_file(copied_file) || fs::file_size(copied_file) != fs::file_size(renamed)) {
        fs::remove_all(root, cleanup_error);
        return 10;
    }

    const auto copy_conflict = engine.copy_item(renamed, copy_destination);
    if (copy_conflict.status != OperationStatus::DestinationConflict || copy_conflict.changed) {
        fs::remove_all(root, cleanup_error);
        return 11;
    }

    const fs::path source_tree = root / "source-tree";
    fs::create_directories(source_tree / "nested");
    if (!write_text(source_tree / "nested" / "data.bin", "tree payload\n")) {
        fs::remove_all(root, cleanup_error);
        return 12;
    }
    const fs::path tree_destination = root / "tree-destination";
    fs::create_directories(tree_destination);
    const auto tree_copy = engine.copy_item(source_tree, tree_destination);
    if (!tree_copy.ok() ||
        !fs::is_regular_file(tree_destination / "source-tree" / "nested" / "data.bin")) {
        fs::remove_all(root, cleanup_error);
        return 13;
    }

    const auto recursive_copy_rejected = engine.copy_item(source_tree, source_tree / "nested");
    if (recursive_copy_rejected.status != OperationStatus::InvalidRequest ||
        recursive_copy_rejected.changed) {
        fs::remove_all(root, cleanup_error);
        return 14;
    }

    const fs::path move_destination = root / "move-destination";
    fs::create_directories(move_destination);
    const auto move_success = engine.move_item(copied_file, move_destination);
    const fs::path moved_file = move_destination / copied_file.filename();
    if (!move_success.ok() || !move_success.changed || fs::exists(copied_file) ||
        !fs::is_regular_file(moved_file)) {
        fs::remove_all(root, cleanup_error);
        return 15;
    }

    const fs::path move_tree_destination = root / "move-tree-destination";
    fs::create_directories(move_tree_destination);
    const auto move_tree = engine.move_item(source_tree, move_tree_destination);
    if (!move_tree.ok() || !move_tree.changed || fs::exists(source_tree) ||
        !fs::is_regular_file(move_tree_destination / "source-tree" / "nested" / "data.bin")) {
        fs::remove_all(root, cleanup_error);
        return 16;
    }

    const fs::path cross_volume_parent = fs::path{"/dev/shm"} /
                                         ("infiltrator-files-cross-volume-" +
                                          std::to_string(::getpid()));
    fs::create_directories(cross_volume_parent, cleanup_error);
    const bool cross_volume_available = !cleanup_error;
    const fs::path cross_volume_source = root / "cross-volume.txt";
    if (cross_volume_available &&
        !write_text(cross_volume_source, "cross-volume payload\n")) {
        fs::remove_all(cross_volume_parent, cleanup_error);
        fs::remove_all(root, cleanup_error);
        return 18;
    }
    struct stat source_stat {};
    struct stat destination_stat {};
    if (cross_volume_available &&
        (::stat(root.c_str(), &source_stat) != 0 ||
        ::stat(cross_volume_parent.c_str(), &destination_stat) != 0 ||
        source_stat.st_dev == destination_stat.st_dev)) {
        // A real second writable filesystem is an environmental qualification,
        // not a correctness failure of the local Release suite.
    } else if (cross_volume_available) {
        const auto cross_volume_move = engine.move_item(cross_volume_source, cross_volume_parent);
        const fs::path cross_volume_destination =
            cross_volume_parent / cross_volume_source.filename();
        if (!cross_volume_move.ok() || !cross_volume_move.changed ||
            fs::exists(cross_volume_source) || !fs::is_regular_file(cross_volume_destination)) {
            fs::remove_all(cross_volume_parent, cleanup_error);
            fs::remove_all(root, cleanup_error);
            return 20;
        }
    }
    fs::remove_all(cross_volume_parent, cleanup_error);

    fs::remove_all(root, cleanup_error);
    return 0;
}
