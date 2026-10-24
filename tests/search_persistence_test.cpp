// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/ui/search_persistence.hpp"
#include "../src/ui/presentation_store.hpp"

#include <gio/gio.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace infiltrator::files;

namespace {
void require(bool condition, const char *reason)
{
    if (!condition) throw std::runtime_error(reason);
}
}
int main()
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("ifm-search-persistence-" + std::to_string(g_get_monotonic_time()));
    try {
        fs::create_directories(root);
        require(g_setenv("XDG_CONFIG_HOME", root.c_str(), TRUE), "Isolation failed");
        require(load_search_privacy().valid, "Missing privacy settings must be usable");
        std::string error;
        require(save_search_privacy({"file:///tmp/private/a", "file:///tmp/private/b"}, error),
                "Private exclusions must persist");
        auto privacy = load_search_privacy();
        require(privacy.valid && privacy.excluded_uris.size() == 2,
                "Two distinct URI exclusions must round-trip");
        require(!privacy_allows_recent_uri(privacy, "file:///tmp/private/a") &&
                !privacy_allows_recent_uri(privacy, "file:///tmp/private/a/subfolder") &&
                privacy_allows_recent_uri(privacy, "file:///tmp/private/ab") &&
                privacy_allows_recent_uri(privacy, "file:///tmp/public"),
                "Activity history must use real URI ancestry, never a raw string prefix");
        require(!save_search_privacy({"relative/path"}, error),
                "Relative/invalid exclusions must be rejected");
        require(load_search_privacy().excluded_uris.size() == 2,
                "Invalid save may not erase existing private exclusions");

        auto list = load_saved_searches();
        SavedSearch sample;
        sample.title = "Project East";
        sample.root_uri = "file:///tmp/sample";
        sample.tag = "Important";
        sample.options.name = "*.log";
        sample.options.content = "critical";
        sample.options.name_mode = SearchNameMode::Glob;
        sample.options.kind = SearchItemKind::RegularFiles;
        sample.options.case_sensitive = true;
        sample.options.recursive = true;
        sample.options.include_hidden = true;
        sample.options.minimum_size = 12;
        sample.options.maximum_size = 10240;
        sample.options.modified_since = 1577836800LL;
        sample.options.modified_before = 4102444800LL;
        sample.options.owner_uid = 1000;
        sample.options.duplicates_by_sha256 = true;
        list.items.push_back(sample);
        require(save_saved_searches(list, error), "Named search persistence failed");
        auto restored = load_saved_searches();
        require(restored.writable && restored.items.size() == 1,
                "Exactly one named search should load");
        const auto &result = restored.items.front();
        require(result.title == sample.title && result.root_uri == sample.root_uri &&
                result.tag == sample.tag && result.options.name == "*.log" &&
                result.options.content == "critical" &&
                result.options.name_mode == SearchNameMode::Glob &&
                result.options.kind == SearchItemKind::RegularFiles &&
                result.options.case_sensitive && result.options.recursive &&
                result.options.include_hidden &&
                result.options.minimum_size == 12 &&
                result.options.maximum_size == 10240 &&
                result.options.modified_since == 1577836800LL &&
                result.options.modified_before == 4102444800LL &&
                result.options.owner_uid == 1000 &&
                result.options.duplicates_by_sha256,
                "Named search must reproduce its complete structured query");
        std::string malformed = "[Saved Searches]\nversion=99\ncount=1\n";
        require(presentation_store::write("saved-searches.ini", malformed, error),
                "Malformed fixture setup failed");
        std::string baseline_search;
        require(presentation_store::read("saved-searches.ini", baseline_search, error) ==
                    presentation_store::ReadStatus::Loaded,
                "Cannot read saved-search corruption fixture");
        restored = load_saved_searches();
        require(!restored.writable &&
                !save_saved_searches(restored, error),
                "Invalid saved-search state must be preserved read-only");
        std::string recovered;
        require(presentation_store::read("saved-searches.ini", recovered, error) ==
                    presentation_store::ReadStatus::Loaded &&
                recovered == baseline_search,
                "Rejected preset writes must preserve original malformed evidence");

        malformed = "[Privacy]\nversion=99\ncount=1\n";
        require(presentation_store::write("search-private.ini", malformed, error),
                "Privacy corruption fixture setup failed");
        std::string baseline_privacy;
        require(presentation_store::read("search-private.ini", baseline_privacy, error) ==
                    presentation_store::ReadStatus::Loaded,
                "Cannot read privacy corruption fixture");
        require(!load_search_privacy().valid, "Invalid privacy file must be detected");
        require(!privacy_allows_recent_uri(load_search_privacy(), "file:///tmp/public"),
                "Unreadable privacy state must stop all recent-location tracking");
        require(!save_search_privacy({}, error),
                "Unparseable privacy state must not be silently cleared");
        require(presentation_store::read("search-private.ini", recovered, error) ==
                    presentation_store::ReadStatus::Loaded &&
                recovered == baseline_privacy,
                "Rejected privacy write must preserve prior bytes");
        fs::remove_all(root);
        return 0;
    } catch (const std::exception &failure) {
        std::cerr << failure.what() << '\n';
        fs::remove_all(root);
        return 1;
    }
}
