// SPDX-License-Identifier: GPL-3.0-or-later
#include "batch_removal_engine.hpp"
#include "operation_journal.hpp"
#include "recovery_operation_engine.hpp"
#include <set>

namespace infiltrator::files {
OperationResult BatchRemovalEngine::execute(const std::vector<std::filesystem::path> &sources,
    const BatchRemovalKind kind, const std::function<bool()> &cancelled,
    const std::function<void()> &item_completed) const
{
    const auto failure = [](OperationStatus status, OperationPhase phase, std::string message, bool changed = false) {
        return OperationResult{status, phase, {}, std::move(message), changed, {}};
    };
    if (sources.empty() || sources.size() > 100000U)
        return failure(OperationStatus::InvalidRequest, OperationPhase::Preflight, "No valid removal selection is available.");
    std::set<std::filesystem::path> unique;
    std::filesystem::path parent;
    for (const auto &source : sources) {
        if (cancelled && cancelled())
            return failure(OperationStatus::Cancelled, OperationPhase::Preflight, "Removal cancelled before changing any item.");
        const auto normalized = source.lexically_normal();
        if (!source.is_absolute() || source != normalized || normalized.filename().empty() || normalized.filename() == "." ||
            normalized.filename() == ".." || !unique.insert(normalized).second ||
            (!parent.empty() && normalized.parent_path() != parent))
            return failure(OperationStatus::InvalidRequest, OperationPhase::Preflight,
                           "Batch removal requires distinct selected items from one folder.");
        parent = normalized.parent_path();
        std::error_code error;
        const auto status = std::filesystem::symlink_status(normalized, error);
        if (error || status.type() == std::filesystem::file_type::not_found)
            return failure(error ? OperationEngine::status_for_error(error) : OperationStatus::InvalidRequest,
                           OperationPhase::Preflight, "A selected item is unavailable. No batch removal was started.");
    }
    OperationJournal journal;
    RecoveryOperationEngine engine;
    std::size_t completed = 0U;
    const char *journal_kind = kind == BatchRemovalKind::Trash ? "batch-trash" : "batch-delete";
    for (const auto &source : sources) {
        const auto detail = "Removal stopped after " + std::to_string(completed) + " of " +
            std::to_string(sources.size()) + " selected items. ";
        if (cancelled && cancelled())
            return failure(OperationStatus::Cancelled, OperationPhase::Execute, detail + "Cancellation requested.", completed != 0U);
        const std::string id = journal.begin(journal_kind, source.string(), {});
        if (id.empty())
            return failure(OperationStatus::VerificationFailure, OperationPhase::Preflight,
                           detail + "The durable journal could not record the next item; that item was not changed.", completed != 0U);
        OperationResult operation = kind == BatchRemovalKind::Trash
            ? engine.trash_item(source) : engine.delete_item(source);
        if (!journal.finish(id, journal_kind, operation)) {
            operation.status = OperationStatus::VerificationFailure;
            operation.phase = OperationPhase::Verify;
            operation.message += " The durable journal could not record completion.";
        }
        if (!operation.ok()) {
            operation.changed = operation.changed || completed != 0U;
            operation.message = detail + operation.message;
            return operation;
        }
        ++completed;
        if (item_completed) item_completed();
    }
    return {OperationStatus::Success, OperationPhase::Complete, {},
        (kind == BatchRemovalKind::Trash ? "Moved to Trash: " : "Permanently deleted: ") +
            std::to_string(completed) + " selected items.", completed != 0U, {}};
}
} // namespace infiltrator::files
