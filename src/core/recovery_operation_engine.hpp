// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "operation_engine.hpp"

#include <filesystem>
#include <string_view>

namespace infiltrator::files {

class TransferControl;

struct RecoveryOperationHooks {
    using ItemOperation = OperationResult (*)(const std::filesystem::path &,
                                               const std::filesystem::path &);
    using BeforeRollback = void (*)(const std::filesystem::path &);

    ItemOperation copy_item{nullptr};
    ItemOperation move_item{nullptr};
    BeforeRollback before_rollback{nullptr};
};

class RecoveryOperationEngine final {
public:
    explicit RecoveryOperationEngine(RecoveryOperationHooks hooks = {}) noexcept
        : hooks_(hooks)
    {
    }

    [[nodiscard]] OperationResult replace_copy(const std::filesystem::path &source,
                                               const std::filesystem::path &destination_parent,
                                               TransferControl *control = nullptr) const;
    [[nodiscard]] OperationResult replace_move(const std::filesystem::path &source,
                                               const std::filesystem::path &destination_parent,
                                               TransferControl *control = nullptr) const;

    [[nodiscard]] OperationResult trash_item(const std::filesystem::path &source) const;
    [[nodiscard]] OperationResult restore_item(std::string_view trash_uri,
                                               const std::filesystem::path &original_path,
                                               bool replace_existing = false) const;

    [[nodiscard]] OperationResult delete_item(const std::filesystem::path &source) const;
    [[nodiscard]] OperationResult delete_uri(std::string_view uri) const;

private:
    RecoveryOperationHooks hooks_{};
};

} // namespace infiltrator::files
