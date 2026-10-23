// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <string_view>

namespace infiltrator::files::presentation_store {
// Small presentation files are bounded, private and atomically replaced through
// Common. A missing file is distinct from malformed or unreadable state.
enum class ReadStatus { Loaded, Missing, Failed };
ReadStatus read(const char *name, std::string &contents, std::string &error);
bool write(const char *name, std::string_view contents, std::string &error);
} // namespace infiltrator::files::presentation_store
