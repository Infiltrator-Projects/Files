// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "operation_engine.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace infiltrator::files {

struct InterruptedOperation {
    std::uint64_t started_millis{0};
    std::string operation_id;
    std::string kind;
    std::string source;
    std::string destination;
};

class OperationJournal final {
public:
    explicit OperationJournal(std::filesystem::path path = default_path());

    [[nodiscard]] std::string begin(std::string_view kind,
                                    std::string_view source,
                                    std::string_view destination) const;
    [[nodiscard]] bool finish(std::string_view operation_id,
                              std::string_view kind,
                              const OperationResult &result) const;

    /**
     * Read a coherent journal snapshot and return operations left open by an
     * earlier application session. START records owned by the current process
     * session are live work, not crash evidence, and are intentionally omitted.
     *
     * @return true when the journal was absent or read coherently; false on an
     *         I/O/locking/format-read failure. `operations` is always cleared
     *         before inspection.
     */
    [[nodiscard]] bool interrupted_operations(
        std::vector<InterruptedOperation> &operations) const;
    [[nodiscard]] std::vector<InterruptedOperation> interrupted_operations() const;
    [[nodiscard]] bool mark_interrupted(const InterruptedOperation &operation) const;

    [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }
    [[nodiscard]] static std::filesystem::path default_path();

private:
    [[nodiscard]] bool append_line(const std::string &line) const;
    [[nodiscard]] bool read_snapshot(std::string &contents) const;
    [[nodiscard]] static std::string escape(std::string_view value);
    [[nodiscard]] static std::string unescape(std::string_view value);

    std::filesystem::path path_;
};

} // namespace infiltrator::files
