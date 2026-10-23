// SPDX-License-Identifier: GPL-3.0-or-later
#include "batch_transfer_engine.hpp"

#include "aggregate_arithmetic.hpp"
#include "operation_journal.hpp"
#include "recovery_operation_engine.hpp"

#include <string>
#include <utility>

namespace infiltrator::files {

namespace {

struct PlannedItem {
    std::filesystem::path source;
    std::filesystem::path destination;
    std::uintmax_t bytes{0};
    std::uintmax_t items{0};
    bool destination_exists{false};
};

OperationResult failure(const OperationStatus status,
                        const OperationPhase phase,
                        const std::filesystem::path &destination,
                        std::string message,
                        const bool changed = false)
{
    return OperationResult{status, phase, destination, std::move(message), changed, {}};
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

bool measure_item(const std::filesystem::path &source,
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

const char *journal_kind(const BatchTransferKind kind) noexcept
{
    return kind == BatchTransferKind::Copy ? "batch-copy" : "batch-move";
}

void advance_unchanged_progress(TransferControl *control, const PlannedItem &item)
{
    if (control == nullptr) {
        return;
    }
    control->add_bytes(item.bytes);
    for (std::uintmax_t index = 0; index < item.items; ++index) {
        control->add_item();
    }
}

} // namespace

OperationResult BatchTransferEngine::execute(
    const std::vector<std::filesystem::path> &sources,
    const std::filesystem::path &destination_parent,
    const BatchTransferKind kind,
    const BatchConflictPolicy conflict_policy,
    TransferControl *control) const
{
    if (sources.empty() || destination_parent.empty()) {
        return failure(OperationStatus::InvalidRequest,
                       OperationPhase::Preflight,
                       {},
                       "The batch source or destination is not available.");
    }

    std::error_code error;
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

    std::vector<PlannedItem> plan;
    plan.reserve(sources.size());
    std::uintmax_t total_bytes = 0;
    std::uintmax_t total_items = 0;

    for (const auto &source : sources) {
        if (source.empty() || source.filename().empty()) {
            return failure(OperationStatus::InvalidRequest,
                           OperationPhase::Preflight,
                           {},
                           "One of the selected items is not available.");
        }
        if (!path_present(source, error)) {
            return failure(error ? OperationEngine::status_for_error(error)
                                 : OperationStatus::InvalidRequest,
                           OperationPhase::Preflight,
                           {},
                           "One of the selected items no longer exists.");
        }

        PlannedItem item;
        item.source = source;
        item.destination = destination_parent / source.filename();
        if (!measure_item(source, item.bytes, item.items, error, control)) {
            if (error == std::errc::operation_canceled)
                return failure(OperationStatus::Cancelled, OperationPhase::Preflight, {},
                               "Batch operation cancelled during preparation.");
            return failure(OperationEngine::status_for_error(error),
                           OperationPhase::Preflight,
                           item.destination,
                           error == std::errc::value_too_large
                               ? "A selected item is too large to represent safely."
                               : "A selected item could not be measured safely.");
        }
        if (!checked_add_uintmax(item.bytes, total_bytes) ||
            !checked_add_uintmax(item.items, total_items)) {
            return failure(OperationStatus::ExecutionFailure,
                           OperationPhase::Preflight,
                           item.destination,
                           "The batch byte or item total is too large to represent safely.");
        }

        if (!(kind == BatchTransferKind::Move && source.parent_path() == destination_parent)) {
            item.destination_exists = path_present(item.destination, error);
            if (error) {
                return failure(OperationEngine::status_for_error(error),
                               OperationPhase::Preflight,
                               item.destination,
                               "A batch destination could not be checked.");
            }
            if (item.destination_exists && conflict_policy == BatchConflictPolicy::Fail) {
                return failure(OperationStatus::DestinationConflict,
                               OperationPhase::Preflight,
                               item.destination,
                               "One or more selected items already exist in the destination.");
            }
        }
        plan.push_back(std::move(item));
    }

    if (control != nullptr) {
        control->begin_batch(total_bytes, total_items);
        if (control->cancelled()) {
            return failure(OperationStatus::Cancelled,
                           OperationPhase::Execute,
                           {},
                           "Batch operation cancelled.");
        }
    }

    TransferOperationEngine transfer;
    RecoveryOperationEngine recovery;
    OperationJournal journal;
    std::size_t changed_count = 0;
    std::size_t skipped_count = 0;

    for (std::size_t index = 0; index < plan.size(); ++index) {
        const PlannedItem &item = plan[index];
        if (control != nullptr && control->cancelled()) {
            return failure(OperationStatus::Cancelled,
                           OperationPhase::Execute,
                           {},
                           "Batch operation cancelled after " + std::to_string(index) + " of " +
                               std::to_string(plan.size()) + " selected items.",
                           changed_count != 0U);
        }

        const std::string id = journal.begin(journal_kind(kind),
                                             item.source.string(),
                                             destination_parent.string());
        if (id.empty()) {
            return failure(OperationStatus::VerificationFailure,
                           OperationPhase::Preflight,
                           item.destination,
                           "The durable operation journal could not record the next batch item, so the batch stopped before mutating it.",
                           changed_count != 0U);
        }

        OperationResult operation;
        const bool use_replace = conflict_policy == BatchConflictPolicy::Replace &&
                                 item.destination_exists;
        if (use_replace) {
            operation = kind == BatchTransferKind::Copy
                            ? recovery.replace_copy(item.source, destination_parent, control)
                            : recovery.replace_move(item.source, destination_parent, control);
        } else {
            ConflictPolicy item_policy = ConflictPolicy::Fail;
            if (conflict_policy == BatchConflictPolicy::Skip) {
                item_policy = ConflictPolicy::Skip;
            } else if (conflict_policy == BatchConflictPolicy::KeepBoth) {
                item_policy = ConflictPolicy::KeepBoth;
            }

            operation = kind == BatchTransferKind::Copy
                            ? transfer.copy_item(item.source, destination_parent, item_policy, control)
                            : transfer.move_item(item.source, destination_parent, item_policy, control);
            if (operation.ok() && !operation.changed) {
                advance_unchanged_progress(control, item);
            }
        }

        if (!journal.finish(id, journal_kind(kind), operation)) {
            if (operation.ok()) {
                operation.status = OperationStatus::VerificationFailure;
                operation.phase = OperationPhase::Verify;
            }
            if (!operation.message.empty()) {
                operation.message += ' ';
            }
            operation.message += "The durable operation journal could not record completion.";
        }

        if (!operation.ok()) {
            operation.changed = operation.changed || changed_count != 0U;
            operation.message = "Batch stopped after " + std::to_string(index) + " of " +
                                std::to_string(plan.size()) + " selected items. " + operation.message;
            return operation;
        }

        if (operation.changed) {
            ++changed_count;
        } else {
            ++skipped_count;
        }
    }

    std::string message = kind == BatchTransferKind::Copy ? "Copied " : "Moved ";
    message += std::to_string(changed_count) +
               (changed_count == 1U ? " selected item" : " selected items");
    if (skipped_count != 0U) {
        message += "; skipped " + std::to_string(skipped_count) +
                   (skipped_count == 1U ? " item" : " items");
    }
    message += '.';

    return OperationResult{OperationStatus::Success,
                           OperationPhase::Complete,
                           {},
                           std::move(message),
                           changed_count != 0U,
                           {}};
}

} // namespace infiltrator::files
