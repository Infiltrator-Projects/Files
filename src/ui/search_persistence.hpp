// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../core/search_engine.hpp"

#include <string>
#include <vector>

namespace infiltrator::files {

// Strict, bounded user-authored search presets and privacy settings. Neither
// persistence layer owns filesystem objects or gives results mutation authority.
struct SavedSearch {
    std::string title;
    std::string root_uri;
    std::string tag;
    SearchOptions options;
};
struct SavedSearchList {
    std::vector<SavedSearch> items;
    bool writable{true};
    std::string error;
};
struct SearchPrivacy {
    std::vector<std::string> excluded_uris;
    bool valid{true};
    std::string error;
};

SavedSearchList load_saved_searches();
bool save_saved_searches(const SavedSearchList &state, std::string &error);
SearchPrivacy load_search_privacy();
bool save_search_privacy(const std::vector<std::string> &uris, std::string &error);

} // namespace infiltrator::files
