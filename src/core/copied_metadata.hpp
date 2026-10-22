// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "destination_ownership.hpp"

namespace infiltrator::files {

// Preserve supported native metadata on an identity-bound private output,
// then flush its data and metadata before publication. Attribute enumeration
// and value storage are bounded; uncertainty fails the transfer, never drops
// attributes silently. Ownership and sparse allocation remain separate policy.
[[nodiscard]] bool preserve_copied_metadata(
    const std::filesystem::path &source,
    const destination_ownership::OwnedOutput &output,
    std::error_code &error);
[[nodiscard]] bool sync_copied_descriptor(int descriptor, std::error_code &error);
[[nodiscard]] bool sync_copy_publication(const std::filesystem::path &parent,
                                          std::error_code &error);

} // namespace infiltrator::files
