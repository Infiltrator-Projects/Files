// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <gio/gio.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace infiltrator::files {

enum class SearchNameMode { Contains, Exact, Glob };
enum class SearchItemKind { Any, RegularFiles, Directories };

struct SearchOptions {
    std::string name;
    SearchNameMode name_mode{SearchNameMode::Contains};
    bool case_sensitive{false};
    bool recursive{true};
    bool include_hidden{false};
    SearchItemKind kind{SearchItemKind::Any};
    std::optional<std::uint64_t> minimum_size;
    std::optional<std::uint64_t> maximum_size;
    std::optional<std::int64_t> modified_since; // Unix seconds, inclusive
    std::string content; // UTF-8 substring, regular text files <=1 MiB only
};

struct SearchHit {
    std::string uri;
    std::string name;
    std::string location;
    bool directory{false};
    std::uint64_t size{0};
};

struct SearchReport {
    std::vector<SearchHit> hits;
    std::uint64_t visited{0};
    std::uint64_t content_skipped{0};
    bool complete{true};
    std::string limitation;
};

// GIO-only bounded search. Calls may block; invoke from an owned worker thread.
// No mutation, symlink traversal, silent cross-mount descent, or silent partial
// result claim. The caller owns a reference to root until this call returns.
SearchReport search_files(GFile *root, const SearchOptions &options,
                          GCancellable *cancel, GError **error);

} // namespace infiltrator::files
