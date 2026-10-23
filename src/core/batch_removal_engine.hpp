// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "operation_engine.hpp"
#include <functional>
#include <vector>

namespace infiltrator::files {
enum class BatchRemovalKind { Trash, PermanentDelete };
class BatchRemovalEngine final {
public:
    // The UI snapshots siblings before confirmation. Each item has its own
    // durable START/END; cancellation is honoured between complete items.
    [[nodiscard]] OperationResult execute(const std::vector<std::filesystem::path> &sources,
        BatchRemovalKind kind, const std::function<bool()> &cancelled = {},
        const std::function<void()> &item_completed = {}) const;
};
} // namespace infiltrator::files
