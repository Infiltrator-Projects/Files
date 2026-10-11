// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "destination_ownership.hpp"
#include "operation_engine.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>

namespace infiltrator::files {

enum class ConflictPolicy {
    Fail,
    Skip,
    KeepBoth,
};

struct TransferProgress {
    std::uintmax_t bytes_done{0};
    std::uintmax_t bytes_total{0};
    std::uintmax_t items_done{0};
    std::uintmax_t items_total{0};
};

class TransferControl final {
public:
    using ProgressObserver = void (*)(TransferControl &, const TransferProgress &) noexcept;

    explicit TransferControl(ProgressObserver observer = nullptr) noexcept
        : observer_(observer)
    {
    }

    void request_cancel() noexcept { cancelled_.store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool cancelled() const noexcept
    {
        return cancelled_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] TransferProgress progress() const noexcept;

    // Progress-reporting contract used by the transfer implementation.
    void begin_batch(std::uintmax_t bytes, std::uintmax_t items) noexcept;
    void set_total(std::uintmax_t bytes, std::uintmax_t items) noexcept;
    void add_bytes(std::uintmax_t bytes) noexcept;
    void add_item() noexcept;

private:
    std::atomic_bool cancelled_{false};
    mutable std::mutex progress_mutex_;
    TransferProgress progress_{};
    bool batch_mode_{false};
    ProgressObserver observer_{nullptr};
};

struct TransferOperationHooks {
    using MoveOwnedOperation = bool (*)(
        const destination_ownership::OwnedOutput &,
        const std::filesystem::path &,
        destination_ownership::OwnedOutput &,
        std::error_code &,
        std::filesystem::path *) noexcept;
    using QuarantineOwnedOperation = bool (*)(
        const destination_ownership::OwnedOutput &,
        destination_ownership::ProtectedStaging &,
        std::filesystem::path &,
        std::error_code &,
        std::filesystem::path *) noexcept;
    using PublishTreeOperation = bool (*)(
        destination_ownership::ProtectedStaging &,
        const std::filesystem::path &,
        const std::filesystem::path &,
        destination_ownership::OwnedOutputs &,
        std::error_code &) noexcept;
    using PublishOutputOperation = bool (*)(
        destination_ownership::ProtectedStaging &,
        const std::filesystem::path &,
        const std::filesystem::path &,
        destination_ownership::OwnedOutput &,
        std::error_code &) noexcept;

    static bool move_owned_default(
        const destination_ownership::OwnedOutput &source,
        const std::filesystem::path &destination,
        destination_ownership::OwnedOutput &moved,
        std::error_code &error,
        std::filesystem::path *retained_quarantine) noexcept
    {
        return destination_ownership::move_owned_no_replace(
            source, destination, moved, error, retained_quarantine);
    }

    MoveOwnedOperation move_owned{move_owned_default};
    QuarantineOwnedOperation quarantine_owned{destination_ownership::quarantine_owned};
    PublishTreeOperation publish_tree{destination_ownership::publish_private_tree};
    PublishOutputOperation publish_output{destination_ownership::publish_private_output};
};

class TransferOperationEngine final {
public:
    explicit TransferOperationEngine(TransferOperationHooks hooks = {}) noexcept
        : hooks_(hooks)
    {
    }

    [[nodiscard]] OperationResult copy_item(const std::filesystem::path &source,
                                            const std::filesystem::path &destination_parent,
                                            ConflictPolicy policy,
                                            TransferControl *control = nullptr) const;
    [[nodiscard]] OperationResult move_item(const std::filesystem::path &source,
                                            const std::filesystem::path &destination_parent,
                                            ConflictPolicy policy,
                                            TransferControl *control = nullptr) const;

    [[nodiscard]] static std::filesystem::path keep_both_destination(
        const std::filesystem::path &source,
        const std::filesystem::path &destination_parent,
        std::error_code &error);

private:
    TransferOperationHooks hooks_{};
};

} // namespace infiltrator::files
