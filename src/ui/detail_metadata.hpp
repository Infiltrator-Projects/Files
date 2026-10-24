// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <gio/gio.h>
#include <infiltratr/temporal_posix.h>

#include <string>
#include <string_view>

namespace infiltrator::files {

[[nodiscard]] std::string detail_type_text(GFileInfo *info);
[[nodiscard]] std::string detail_size_text(GFileInfo *info);
[[nodiscard]] std::string detail_modified_text(
    GFileInfo *info, InfiltratrTemporalDateProvider *date_provider = nullptr);

// Testable seam for the POSIX locale time pattern used by Standard time. The
// resulting strftime pattern keeps the locale's field order, separators and
// localized unit text while removing only the seconds field selected off by
// the shared System Settings temporal policy.
[[nodiscard]] std::string detail_locale_time_format_without_seconds(
    std::string_view time_format,
    std::string_view ampm_format);

// Raw metadata comparators for GtkColumnView sorting. They deliberately compare
// underlying values rather than the human-facing formatted strings.
[[nodiscard]] int detail_compare_name(GFileInfo *left, GFileInfo *right);
[[nodiscard]] int detail_compare_type(GFileInfo *left, GFileInfo *right);
[[nodiscard]] int detail_compare_size(GFileInfo *left, GFileInfo *right);
[[nodiscard]] int detail_compare_modified(GFileInfo *left, GFileInfo *right);
[[nodiscard]] int detail_compare_created(GFileInfo *left, GFileInfo *right);
[[nodiscard]] std::string detail_created_text(
    GFileInfo *info, InfiltratrTemporalDateProvider *date_provider = nullptr);
[[nodiscard]] std::string detail_visual_label(GFileInfo *info, bool show_extensions);

} // namespace infiltrator::files
