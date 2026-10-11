// SPDX-License-Identifier: GPL-3.0-or-later
#include "recovery_operation_engine.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <unistd.h>

using infiltrator::files::OperationStatus;
using infiltrator::files::OperationPhase;
using infiltrator::files::OperationResult;
using infiltrator::files::RecoveryOperationEngine;
using infiltrator::files::RecoveryOperationHooks;

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

unsigned int injected_operation_calls = 0U;
bool remove_source_on_replacement = false;

OperationResult conflict_then_failure(const std::filesystem::path &source,
                                      const std::filesystem::path &destination_parent)
{
    const std::filesystem::path destination = destination_parent / source.filename();
    ++injected_operation_calls;
    if (injected_operation_calls == 1U) {
        return OperationResult{OperationStatus::DestinationConflict,
                               OperationPhase::Preflight,
                               destination,
                               "Injected destination conflict.",
                               false,
                               {}};
    }
    if (remove_source_on_replacement) {
        std::error_code error;
        std::filesystem::remove(source, error);
    }
    return OperationResult{OperationStatus::ExecutionFailure,
                           OperationPhase::Execute,
                           destination,
                           "Injected replacement failure.",
                           false,
                           {}};
}

void claim_rollback_destination(const std::filesystem::path &destination)
{
    (void)write_text(destination, "competing rollback claimant\n");
}

} // namespace

int main()
{
    namespace fs = std::filesystem;

    const fs::path root = fs::temp_directory_path() /
                          ("infiltrator-files-recovery-test-" + std::to_string(::getpid()));
    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
    fs::create_directories(root);

    RecoveryOperationEngine engine;

    const fs::path copy_source_dir = root / "copy-source";
    const fs::path copy_destination_dir = root / "copy-destination";
    fs::create_directories(copy_source_dir);
    fs::create_directories(copy_destination_dir);
    const fs::path copy_source = copy_source_dir / "same.txt";
    const fs::path copy_destination = copy_destination_dir / "same.txt";
    if (!write_text(copy_source, "new-copy\n") ||
        !write_text(copy_destination, "old-copy\n")) {
        fs::remove_all(root, cleanup_error);
        return 1;
    }

    const auto replaced_copy = engine.replace_copy(copy_source, copy_destination_dir);
    if (!replaced_copy.ok() || !replaced_copy.changed || !fs::exists(copy_source) ||
        read_text(copy_destination) != "new-copy\n") {
        fs::remove_all(root, cleanup_error);
        return 2;
    }

    for (const auto &entry : fs::directory_iterator(copy_destination_dir)) {
        if (entry.path().filename().string().starts_with(".infiltrator-replace-")) {
            fs::remove_all(root, cleanup_error);
            return 3;
        }
    }

    const fs::path move_source_dir = root / "move-source";
    const fs::path move_destination_dir = root / "move-destination";
    fs::create_directories(move_source_dir);
    fs::create_directories(move_destination_dir);
    const fs::path move_source = move_source_dir / "same.txt";
    const fs::path move_destination = move_destination_dir / "same.txt";
    if (!write_text(move_source, "new-move\n") ||
        !write_text(move_destination, "old-move\n")) {
        fs::remove_all(root, cleanup_error);
        return 4;
    }

    const auto replaced_move = engine.replace_move(move_source, move_destination_dir);
    if (!replaced_move.ok() || !replaced_move.changed || fs::exists(move_source) ||
        read_text(move_destination) != "new-move\n") {
        fs::remove_all(root, cleanup_error);
        return 5;
    }

    const fs::path delete_tree = root / "delete-me";
    fs::create_directories(delete_tree / "nested");
    if (!write_text(delete_tree / "nested" / "payload.txt", "delete\n")) {
        fs::remove_all(root, cleanup_error);
        return 6;
    }

    const auto deleted = engine.delete_item(delete_tree);
    if (!deleted.ok() || !deleted.changed || fs::exists(delete_tree)) {
        fs::remove_all(root, cleanup_error);
        return 7;
    }

    const auto missing_delete = engine.delete_item(delete_tree);
    if (missing_delete.status != OperationStatus::InvalidRequest || missing_delete.changed) {
        fs::remove_all(root, cleanup_error);
        return 8;
    }

    const fs::path rollback_copy_source_dir = root / "rollback-copy-source";
    const fs::path rollback_copy_destination_dir = root / "rollback-copy-destination";
    fs::create_directories(rollback_copy_source_dir);
    fs::create_directories(rollback_copy_destination_dir);
    const fs::path rollback_copy_source = rollback_copy_source_dir / "blocked.txt";
    const fs::path rollback_copy_destination =
        rollback_copy_destination_dir / rollback_copy_source.filename();
    if (!write_text(rollback_copy_source, "new copy\n") ||
        !write_text(rollback_copy_destination, "previous copy\n")) {
        fs::remove_all(root, cleanup_error);
        return 9;
    }
    RecoveryOperationHooks rollback_copy_hooks;
    rollback_copy_hooks.copy_item = conflict_then_failure;
    rollback_copy_hooks.before_rollback = claim_rollback_destination;
    RecoveryOperationEngine rollback_copy_engine{rollback_copy_hooks};
    injected_operation_calls = 0U;
    remove_source_on_replacement = false;
    const auto rollback_copy = rollback_copy_engine.replace_copy(
        rollback_copy_source, rollback_copy_destination_dir);
    if (rollback_copy.status != OperationStatus::VerificationFailure ||
        !rollback_copy.changed || rollback_copy.retained_paths.size() != 1U ||
        !fs::exists(rollback_copy.retained_paths.front()) ||
        read_text(rollback_copy.retained_paths.front()) != "previous copy\n" ||
        read_text(rollback_copy_destination) != "competing rollback claimant\n" ||
        rollback_copy.message.find(rollback_copy.retained_paths.front().string()) ==
            std::string::npos) {
        fs::remove_all(root, cleanup_error);
        return 10;
    }

    const fs::path rollback_move_source_dir = root / "rollback-move-source";
    const fs::path rollback_move_destination_dir = root / "rollback-move-destination";
    fs::create_directories(rollback_move_source_dir);
    fs::create_directories(rollback_move_destination_dir);
    const fs::path rollback_move_source = rollback_move_source_dir / "blocked.txt";
    const fs::path rollback_move_destination =
        rollback_move_destination_dir / rollback_move_source.filename();
    if (!write_text(rollback_move_source, "new move\n") ||
        !write_text(rollback_move_destination, "previous move\n")) {
        fs::remove_all(root, cleanup_error);
        return 11;
    }
    RecoveryOperationHooks rollback_move_hooks;
    rollback_move_hooks.move_item = conflict_then_failure;
    rollback_move_hooks.before_rollback = claim_rollback_destination;
    RecoveryOperationEngine rollback_move_engine{rollback_move_hooks};
    injected_operation_calls = 0U;
    remove_source_on_replacement = false;
    const auto rollback_move = rollback_move_engine.replace_move(
        rollback_move_source, rollback_move_destination_dir);
    if (rollback_move.status != OperationStatus::VerificationFailure ||
        !rollback_move.changed || rollback_move.retained_paths.size() != 1U ||
        !fs::exists(rollback_move.retained_paths.front()) ||
        read_text(rollback_move.retained_paths.front()) != "previous move\n" ||
        read_text(rollback_move_destination) != "competing rollback claimant\n" ||
        rollback_move.message.find(rollback_move.retained_paths.front().string()) ==
            std::string::npos) {
        fs::remove_all(root, cleanup_error);
        return 12;
    }

    const fs::path uncertain_source_dir = root / "uncertain-move-source";
    const fs::path uncertain_destination_dir = root / "uncertain-move-destination";
    fs::create_directories(uncertain_source_dir);
    fs::create_directories(uncertain_destination_dir);
    const fs::path uncertain_source = uncertain_source_dir / "uncertain.txt";
    const fs::path uncertain_destination =
        uncertain_destination_dir / uncertain_source.filename();
    if (!write_text(uncertain_source, "uncertain new\n") ||
        !write_text(uncertain_destination, "uncertain previous\n")) {
        fs::remove_all(root, cleanup_error);
        return 13;
    }
    RecoveryOperationHooks uncertain_hooks;
    uncertain_hooks.move_item = conflict_then_failure;
    RecoveryOperationEngine uncertain_engine{uncertain_hooks};
    injected_operation_calls = 0U;
    remove_source_on_replacement = true;
    const auto uncertain = uncertain_engine.replace_move(
        uncertain_source, uncertain_destination_dir);
    remove_source_on_replacement = false;
    if (uncertain.status != OperationStatus::VerificationFailure ||
        !uncertain.changed || uncertain.retained_paths.size() != 1U ||
        !fs::exists(uncertain.retained_paths.front()) ||
        read_text(uncertain.retained_paths.front()) != "uncertain previous\n" ||
        uncertain.message.find(uncertain.retained_paths.front().string()) ==
            std::string::npos) {
        fs::remove_all(root, cleanup_error);
        return 14;
    }

    fs::remove_all(root, cleanup_error);
    return 0;
}
