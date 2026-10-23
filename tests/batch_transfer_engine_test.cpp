// SPDX-License-Identifier: GPL-3.0-or-later
#include "batch_transfer_engine.hpp"
#include "operation_journal.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

using infiltrator::files::BatchConflictPolicy;
using infiltrator::files::BatchTransferEngine;
using infiltrator::files::BatchTransferKind;
using infiltrator::files::OperationJournal;
using infiltrator::files::OperationStatus;
using infiltrator::files::TransferControl;

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
                          ("infiltrator-files-batch-test-" + std::to_string(::getpid()));
    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
    fs::create_directories(root / "source");
    fs::create_directories(root / "destination");
    fs::create_directories(root / "state");
    if (::setenv("XDG_STATE_HOME", (root / "state").c_str(), 1) != 0) {
        return 1;
    }

    const fs::path alpha = root / "source" / "alpha.txt";
    const fs::path beta = root / "source" / "beta.txt";
    if (!write_text(alpha, "alpha\n") || !write_text(beta, "beta\n") ||
        !write_text(root / "destination" / "alpha.txt", "existing\n")) {
        fs::remove_all(root, cleanup_error);
        return 2;
    }

    const std::vector<fs::path> sources{alpha, beta};
    BatchTransferEngine engine;

    const auto conflict = engine.execute(sources,
                                         root / "destination",
                                         BatchTransferKind::Copy,
                                         BatchConflictPolicy::Fail);
    if (conflict.status != OperationStatus::DestinationConflict || conflict.changed ||
        fs::exists(root / "destination" / "beta.txt")) {
        fs::remove_all(root, cleanup_error);
        return 3;
    }

    TransferControl skip_control;
    const auto skipped = engine.execute(sources,
                                        root / "destination",
                                        BatchTransferKind::Copy,
                                        BatchConflictPolicy::Skip,
                                        &skip_control);
    if (!skipped.ok() || !skipped.changed ||
        !fs::exists(root / "destination" / "beta.txt") ||
        !fs::exists(alpha) || !fs::exists(beta)) {
        fs::remove_all(root, cleanup_error);
        return 4;
    }
    const auto skip_progress = skip_control.progress();
    if (skip_progress.items_total == 0U ||
        skip_progress.items_done != skip_progress.items_total ||
        skip_progress.bytes_done != skip_progress.bytes_total) {
        fs::remove_all(root, cleanup_error);
        return 5;
    }

    fs::create_directories(root / "keep-destination");
    if (!write_text(root / "keep-destination" / "alpha.txt", "occupied\n")) {
        fs::remove_all(root, cleanup_error);
        return 6;
    }
    const auto kept = engine.execute(sources,
                                     root / "keep-destination",
                                     BatchTransferKind::Copy,
                                     BatchConflictPolicy::KeepBoth);
    if (!kept.ok() ||
        !fs::exists(root / "keep-destination" / "alpha (copy).txt") ||
        !fs::exists(root / "keep-destination" / "beta.txt")) {
        fs::remove_all(root, cleanup_error);
        return 7;
    }

    const fs::path move_one = root / "source" / "move-one.txt";
    const fs::path move_two = root / "source" / "move-two.txt";
    if (!write_text(move_one, "one\n") || !write_text(move_two, "two\n")) {
        fs::remove_all(root, cleanup_error);
        return 8;
    }
    fs::create_directories(root / "move-destination");
    const auto moved = engine.execute({move_one, move_two},
                                      root / "move-destination",
                                      BatchTransferKind::Move,
                                      BatchConflictPolicy::Fail);
    if (!moved.ok() || fs::exists(move_one) || fs::exists(move_two) ||
        !fs::exists(root / "move-destination" / "move-one.txt") ||
        !fs::exists(root / "move-destination" / "move-two.txt")) {
        fs::remove_all(root, cleanup_error);
        return 9;
    }

    fs::create_directories(root / "cancel-destination");
    TransferControl cancelled_control;
    cancelled_control.request_cancel();
    const auto cancelled = engine.execute(sources,
                                          root / "cancel-destination",
                                          BatchTransferKind::Copy,
                                          BatchConflictPolicy::Fail,
                                          &cancelled_control);
    if (cancelled.status != OperationStatus::Cancelled || cancelled.changed ||
        fs::exists(root / "cancel-destination" / "alpha.txt") ||
        fs::exists(root / "cancel-destination" / "beta.txt")) {
        fs::remove_all(root, cleanup_error);
        return 10;
    }

    OperationJournal journal;
    if (!journal.interrupted_operations().empty()) {
        fs::remove_all(root, cleanup_error);
        return 11;
    }

    TransferControl replace_control;
    const auto replaced = engine.execute(sources, root / "destination", BatchTransferKind::Copy,
                                         BatchConflictPolicy::Replace, &replace_control);
    const auto progress = replace_control.progress();
    if (!replaced.ok() || !replaced.changed || progress.bytes_total != 11U ||
        progress.bytes_done != progress.bytes_total || progress.items_total != 2U ||
        progress.items_done != progress.items_total) return 12;

    TransferControl replacement_cancel(+[](TransferControl &control,
                                           const infiltrator::files::TransferProgress &p) noexcept {
        if (p.bytes_done != 0U) control.request_cancel();
    });
    write_text(root / "destination" / "alpha.txt", "old alpha");
    write_text(root / "destination" / "beta.txt", "old beta");
    const auto stopped = engine.execute(sources, root / "destination", BatchTransferKind::Copy,
                                        BatchConflictPolicy::Replace, &replacement_cancel);
    std::ifstream old_alpha(root / "destination" / "alpha.txt");
    std::ifstream old_beta(root / "destination" / "beta.txt");
    if (stopped.status != OperationStatus::Cancelled || !stopped.changed ||
        stopped.retained_paths.size() != 1U || !fs::exists(stopped.retained_paths.front()) ||
        std::string(std::istreambuf_iterator<char>(old_alpha), {}) != "old alpha" ||
        std::string(std::istreambuf_iterator<char>(old_beta), {}) != "old beta" ||
        !fs::exists(alpha) || !fs::exists(beta) || !journal.interrupted_operations().empty()) return 13;

    fs::remove_all(root, cleanup_error);
    return 0;
}
