// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "operation_engine.hpp"
#include <gio/gio.h>
#include <filesystem>
#include <string>

namespace infiltrator::files {

// Native archive extraction into a fresh output folder. The archive is
// untrusted; no entry may create a link, device or path outside private staging.
class ArchiveEngine final {
public:
    [[nodiscard]] OperationResult extract(const std::filesystem::path &archive_path,
                                          const std::filesystem::path &destination_parent,
                                          const std::string &journal_id,
                                          GCancellable *cancel = nullptr) const;
};

} // namespace infiltrator::files
