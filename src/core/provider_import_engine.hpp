// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "operation_engine.hpp"

#include <gio/gio.h>
#include <filesystem>
#include <string>

namespace infiltrator::files {

// Copy-only ingress from a GIO provider into a native location. No device
// mutation, no implicit GIO move, and no direct publication of unverified data.
class ProviderImportEngine final {
public:
    [[nodiscard]] OperationResult import_regular_file(
        const std::string &uri, const std::filesystem::path &destination_parent,
        const std::string &journal_id, GCancellable *cancel = nullptr) const;
};

} // namespace infiltrator::files
