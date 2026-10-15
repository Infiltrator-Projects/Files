// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <limits>

namespace infiltrator::files {

// Common 1.19.41 provides checked signed addition and unsigned multiply/
// subtract, but no checked uint64/uintmax addition. File-operation byte and
// item totals require the full uintmax_t range, so this is the single local
// authority until Common exposes an equally strong unsigned-add contract.
[[nodiscard]] inline bool checked_add_uintmax(const std::uintmax_t value,
                                              std::uintmax_t &total) noexcept
{
    if (value > std::numeric_limits<std::uintmax_t>::max() - total) {
        return false;
    }
    total += value;
    return true;
}

} // namespace infiltrator::files
