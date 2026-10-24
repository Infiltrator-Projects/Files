// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/core/search_engine.hpp"

#include <gio/gio.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <cstdint>
#include <unistd.h>

using infiltrator::files::SearchHit;
using infiltrator::files::SearchNameMode;
using infiltrator::files::SearchOptions;
using infiltrator::files::SearchItemKind;
using infiltrator::files::SearchReport;
using infiltrator::files::search_files;

namespace {
void require(bool ok, const char *message)
{
    if (!ok) throw std::runtime_error(message);
}

void write(const std::filesystem::path &file, const std::string &text)
{
    std::ofstream output(file, std::ios::binary);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(output.good(), "Could not construct search fixture");
}

SearchReport run(GFile *root, const SearchOptions &options)
{
    GCancellable *cancel = g_cancellable_new();
    GError *error = nullptr;
    SearchReport report = search_files(root, options, cancel, &error);
    g_object_unref(cancel);
    if (error != nullptr) {
        const std::string message = error->message;
        g_error_free(error);
        throw std::runtime_error("Search failed: " + message);
    }
    return report;
}

bool found(const SearchReport &report, const std::string &name)
{
    for (const SearchHit &hit : report.hits) {
        if (hit.name == name) return true;
    }
    return false;
}
}

int main()
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("ifm-recursive-search-" + std::to_string(g_get_monotonic_time()));
    try {
        fs::create_directories(root / "nested" / "more");
        write(root / "Alpha.TXT", "Distinctive blue orchid\n");
        write(root / "notes.txt", "notes\n");
        write(root / "nested" / "same-notes.txt", "notes\n");
        write(root / "small.txt", "x");
        write(root / "nested" / "more" / "recursed.txt", "deep cobalt text\n");
        write(root / "hidden.txt", "visible name\n");
        write(root / "archive.bin", std::string("nul\0binary", 10));
        fs::create_symlink(root, root / "nested" / "cycle");
        GFile *location = g_file_new_for_path(root.c_str());

        SearchOptions o;
        o.name = "recursed";
        auto report = run(location, o);
        require(report.complete && report.hits.size() == 1 &&
                found(report, "recursed.txt"), "Recursive filename search must find nested result");
        require(report.visited < 30, "Symlink cycles must not be followed");
        require(report.hits.front().uri.find("file://") == 0,
                "Results must retain provider URIs");

        o.recursive = false;
        require(run(location, o).hits.empty(), "Current-folder search must not recurse");
        o.recursive = true;
        o.name = "alpha.txt";
        o.name_mode = SearchNameMode::Exact;
        require(found(run(location, o), "Alpha.TXT"), "Exact case-folded name did not match");
        o.case_sensitive = true;
        require(run(location, o).hits.empty(), "Exact case-sensitive name must reject other case");
        o.name = "*.TXT";
        o.name_mode = SearchNameMode::Glob;
        require(found(run(location, o), "Alpha.TXT"),
                "Case-sensitive wildcard must include uppercase extension");
        require(!found(run(location, o), "notes.txt"),
                "Case-sensitive wildcard must exclude lowercase extension");
        o.case_sensitive = false;
        require(found(run(location, o), "notes.txt"),
                "Case-folded wildcard must include lowercase extension");

        o = SearchOptions{};
        o.kind = SearchItemKind::Directories;
        require(found(run(location, o), "nested") &&
                !found(run(location, o), "notes.txt"),
                "Directory type filter must exclude regular files");
        o.kind = SearchItemKind::RegularFiles;
        require(!found(run(location, o), "nested") &&
                found(run(location, o), "notes.txt"),
                "Regular-file type filter must exclude folders");

        o = SearchOptions{};
        o.name = "small.txt";
        o.minimum_size = 2;
        require(run(location, o).hits.empty(),
                "Minimum size filter must exclude smaller files");
        o.minimum_size.reset();
        o.maximum_size = 1;
        require(found(run(location, o), "small.txt"),
                "Maximum size filter must include exact boundary");
        o.maximum_size = 0;
        require(run(location, o).hits.empty(), "Maximum size zero must remain a real bound");

        o = SearchOptions{};
        o.name = "Alpha";
        o.modified_since = 4102444800LL; // 2100: newer than any fixture
        require(run(location, o).hits.empty(), "Modified-since filter must apply");
        o.modified_since = 946684800LL;
        require(found(run(location, o), "Alpha.TXT"),
                "Modified-since must include newer files");

        o = SearchOptions{};
        o.content = "BLUE ORCHID";
        require(found(run(location, o), "Alpha.TXT") &&
                !found(run(location, o), "notes.txt"),
                "Case-insensitive text search must inspect regular text files");
        o.case_sensitive = true;
        require(!found(run(location, o), "Alpha.TXT"),
                "Case-sensitive text search must not fold content");
        o.content = "blue orchid";
        require(found(run(location, o), "Alpha.TXT"),
                "Exact-case content search must succeed");

        // Catalogue #366: duplicates are size+digest matches, without
        // pretending digest equality is proof of byte identity.
        o = SearchOptions{};
        o.duplicates_by_sha256 = true;
        const auto duplicates = run(location, o);
        require(duplicates.complete && duplicates.hits.size() == 2 &&
                found(duplicates, "notes.txt") && found(duplicates, "same-notes.txt"),
                "Duplicate search must return only genuine matching digest groups");
        o.recursive = false;
        require(run(location, o).hits.empty(),
                "Duplicate search must respect non-recursive scope");

        // Catalogue #359: filter by native UID without making unknown equal zero.
        o = SearchOptions{};
        o.name = "notes.txt";
        o.owner_uid = static_cast<std::uint32_t>(getuid());
        require(found(run(location, o), "notes.txt"),
                "Owner UID must match the provider's actual native attribute");
        o.owner_uid = static_cast<std::uint32_t>(getuid() + 1U);
        require(run(location, o).hits.empty(),
                "Owner UID filter must reject a different native owner");

        // Catalogue #397: inclusive start, exclusive end, including invalid
        // intervals and missing provider timestamp safeguards.
        o = SearchOptions{};
        o.name = "notes.txt";
        o.modified_before = 946684800LL;
        require(run(location, o).hits.empty(), "Old date upper bound must exclude new files");
        o.modified_before = 4102444800LL;
        require(found(run(location, o), "notes.txt"), "Future upper bound must admit new files");
        o.modified_since = 4102444800LL;
        GCancellable *invalid_cancel = g_cancellable_new();
        GError *invalid_error = nullptr;
        (void)search_files(location, o, invalid_cancel, &invalid_error);
        require(g_error_matches(invalid_error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT),
                "An empty modified interval must be rejected before enumeration");
        g_clear_error(&invalid_error);
        g_object_unref(invalid_cancel);

        // Catalogue #663: an exclusion must reject an exact path subtree,
        // never an unrelated pathname with a shared textual prefix.
        o = SearchOptions{};
        GFile *hidden = g_file_new_for_path((root / "nested").c_str());
        gchar *blocked_uri = g_file_get_uri(hidden);
        o.excluded_uris.emplace_back(blocked_uri);
        g_free(blocked_uri);
        g_object_unref(hidden);
        require(!found(run(location, o), "recursed.txt") &&
                found(run(location, o), "notes.txt"),
                "Excluded subtrees must be pruned by GFile identity");
        o.name = "notes.txt";
        require(found(run(location, o), "notes.txt"), "Exclusion must not suppress siblings");

        o = SearchOptions{};
        o.name = "Alpha";
        o.minimum_size = 10;
        o.maximum_size = 2;
        GCancellable *cancel = g_cancellable_new();
        GError *error = nullptr;
        (void)search_files(location, o, cancel, &error);
        require(g_error_matches(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT),
                "Inconsistent size constraints must fail before enumeration");
        g_clear_error(&error);
        o.minimum_size.reset();
        o.maximum_size.reset();
        g_cancellable_cancel(cancel);
        (void)search_files(location, o, cancel, &error);
        require(g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED),
                "Canceled searches must report cancellation, not empty success");
        g_clear_error(&error);
        g_object_unref(cancel);
        g_object_unref(location);
        fs::remove_all(root);
        return 0;
    } catch (const std::exception &failure) {
        std::cerr << failure.what() << '\n';
        fs::remove_all(root);
        return 1;
    }
}
