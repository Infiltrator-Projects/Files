// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/ui/tag_catalogue.hpp"
#include "../src/ui/presentation_store.hpp"

#include <gio/gio.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace infiltrator::files;

namespace {
void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
}

int main()
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("ifm-tags-" + std::to_string(g_get_monotonic_time()));
    try {
        fs::create_directories(root);
        require(g_setenv("XDG_CONFIG_HOME", root.c_str(), TRUE),
                "Could not isolate the test's private config directory");
        const auto initial = load_tag_catalogue();
        require(initial.writable && initial.items.empty(), "Missing tag state must load empty");
        require(valid_tag("Project East") && valid_tag("Índice") &&
                !valid_tag("with,comma") && !valid_tag(""),
                "Tag validation must enforce unambiguous bounded labels");
        std::string error;
        const std::vector<std::string> files = {
            "file:///tmp/tagged-file-A", "file:///tmp/tagged-file-B"};
        require(apply_tags(files, {"Project East", "Important"}, error),
                "Multi-object tag assignment must persist atomically");
        auto saved = load_tag_catalogue();
        require(saved.writable && saved.items.size() == 2,
                "Both tagged URIs must persist");
        require(uris_for_tag(saved, "Project East").size() == 2,
                "Derived tag lookup must return both associated objects");
        require(export_tags_tsv(saved).find("file:///tmp/tagged-file-B") != std::string::npos,
                "Tag database must have an exportable form");
        require(apply_tags({files.front()}, {}, error),
                "Tag removal must not mutate the underlying file");
        saved = load_tag_catalogue();
        require(saved.items.size() == 1 &&
                tags_for(saved, files.front()).empty(),
                "Clearing one item's labels must preserve unrelated records");
        std::string broken = "[Tags]\nversion=1\ncount=1\n";
        require(presentation_store::write("tags.ini", broken, error),
                "Fixture corruption setup failed");
        std::string written_fixture;
        require(presentation_store::read("tags.ini", written_fixture, error) ==
                    presentation_store::ReadStatus::Loaded,
                "Cannot read back corrupted catalogue fixture");
        const auto malformed = load_tag_catalogue();
        require(!malformed.writable && malformed.items.empty(),
                "Malformed tag database must disable writes");
        require(!apply_tags(files, {"should-not-replace"}, error),
                "Malformed catalogue must be preserved, not overwritten");
        std::string retained;
        require(presentation_store::read("tags.ini", retained, error) ==
                    presentation_store::ReadStatus::Loaded && retained == written_fixture,
                "Rejected changes must preserve the exact unreadable database");
        fs::remove_all(root);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        fs::remove_all(root);
        return 1;
    }
}
