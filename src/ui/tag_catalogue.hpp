// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace infiltrator::files {

// Explicitly derived catalogue, not filesystem identity. URI associations may
// become stale following external moves/renames and never authorize mutations.
struct TagCatalogue {
    std::map<std::string, std::set<std::string>> items;
    bool writable{true};
    std::string error;
};

bool valid_tag(const std::string &name);
TagCatalogue load_tag_catalogue();
bool save_tag_catalogue(const TagCatalogue &value, std::string &error);
bool apply_tags(const std::vector<std::string> &uris,
                const std::set<std::string> &labels, std::string &error);
std::set<std::string> tags_for(const TagCatalogue &tags, const std::string &uri);
std::vector<std::string> uris_for_tag(const TagCatalogue &tags, const std::string &label);
std::string export_tags_tsv(const TagCatalogue &tags);
// Rebuild the derived catalogue from an exact bounded exported TSV snapshot.
// Validation is all-or-nothing; corrupt existing state is never overwritten.
bool import_tags_tsv(std::string_view input, std::string &error);

} // namespace infiltrator::files
