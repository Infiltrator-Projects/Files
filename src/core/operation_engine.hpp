// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace infiltrator::files {

enum class OperationPhase {
    Preflight,
    Execute,
    Verify,
    Complete,
};

enum class OperationStatus {
    Success,
    InvalidRequest,
    DestinationConflict,
    PermissionFailure,
    ReadOnlyLocation,
    UnsupportedCapability,
    Cancelled,
    ExecutionFailure,
    VerificationFailure,
};

struct OperationResult {
    OperationStatus status{OperationStatus::ExecutionFailure};
    OperationPhase phase{OperationPhase::Preflight};
    std::filesystem::path destination;
    std::string message;
    bool changed{false};
    std::vector<std::filesystem::path> retained_paths;

    [[nodiscard]] bool ok() const noexcept { return status == OperationStatus::Success; }
};

class OperationEngine final {
public:
    [[nodiscard]] OperationResult create_directory(const std::filesystem::path &parent,
                                                   std::string_view name) const;
    [[nodiscard]] OperationResult create_empty_file(const std::filesystem::path &parent,
                                                    std::string_view name) const;
    // Linux-native mode changes are descriptor-bound and never follow the final
    // component through a symlink. The UI/journal owns operation admission.
    [[nodiscard]] OperationResult change_permissions(const std::filesystem::path &source,
                                                      unsigned int permissions) const;
    // Create a named native link beside the selected object. Both forms use
    // atomic no-replace namespace insertion and report durability failures.
    [[nodiscard]] OperationResult create_symbolic_link(const std::filesystem::path &source,
                                                        std::string_view name) const;
    [[nodiscard]] OperationResult create_hard_link(const std::filesystem::path &source,
                                                    std::string_view name) const;
    [[nodiscard]] OperationResult rename_item(const std::filesystem::path &source,
                                              std::string_view new_name) const;
    [[nodiscard]] OperationResult copy_item(const std::filesystem::path &source,
                                            const std::filesystem::path &destination_parent) const;
    [[nodiscard]] OperationResult move_item(const std::filesystem::path &source,
                                            const std::filesystem::path &destination_parent) const;

    [[nodiscard]] static bool validate_item_name(std::string_view name,
                                                 std::string *reason = nullptr);
    [[nodiscard]] static bool validate_directory_name(std::string_view name,
                                                      std::string *reason = nullptr);
    [[nodiscard]] static OperationStatus status_for_error(const std::error_code &error) noexcept;
};

} // namespace infiltrator::files
