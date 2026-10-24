// SPDX-License-Identifier: GPL-3.0-or-later
#include "search_engine.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace infiltrator::files {
namespace {
constexpr std::uint64_t kMaxVisited = 100000;
constexpr std::size_t kMaxResults = 500;
constexpr std::size_t kMaxDepth = 64;
constexpr goffset kMaxTextBytes = 1024 * 1024;
constexpr const char *kAttributes =
    "standard::name,standard::display-name,standard::type,standard::size,"
    "standard::is-hidden,standard::content-type,time::modified,unix::device,"
    "unix::uid";

std::string folded(const std::string &input)
{
    char *value = g_utf8_casefold(input.c_str(), -1);
    std::string out = value != nullptr ? value : "";
    g_free(value);
    return out;
}

bool matches_name(const std::string &name, const SearchOptions &o)
{
    if (o.name.empty()) return true;
    const std::string candidate = o.case_sensitive ? name : folded(name);
    const std::string pattern = o.case_sensitive ? o.name : folded(o.name);
    switch (o.name_mode) {
    case SearchNameMode::Contains:
        return candidate.find(pattern) != std::string::npos;
    case SearchNameMode::Exact:
        return candidate == pattern;
    case SearchNameMode::Glob:
        return g_pattern_match_simple(pattern.c_str(), candidate.c_str());
    }
    return false;
}

bool includes_text(GFile *file, const SearchOptions &options,
                   GCancellable *cancel, SearchReport &report, GError **error)
{
    if (options.content.empty()) return true;
    GFileInputStream *stream = g_file_read(file, cancel, error);
    if (stream == nullptr) return false;
    std::string payload;
    payload.reserve(16384);
    std::array<char, 65536> buffer{};
    while (payload.size() <= static_cast<std::size_t>(kMaxTextBytes)) {
        const gssize bytes = g_input_stream_read(
            G_INPUT_STREAM(stream), buffer.data(), buffer.size(), cancel, error);
        if (bytes <= 0) break;
        payload.append(buffer.data(), static_cast<std::size_t>(bytes));
    }
    g_input_stream_close(G_INPUT_STREAM(stream), nullptr, nullptr);
    g_object_unref(stream);
    if (error != nullptr && *error != nullptr) return false;
    if (payload.size() > static_cast<std::size_t>(kMaxTextBytes) ||
        !g_utf8_validate(payload.c_str(), static_cast<gssize>(payload.size()), nullptr)) {
        ++report.content_skipped;
        return false;
    }
    const std::string haystack = options.case_sensitive ? payload : folded(payload);
    const std::string needle = options.case_sensitive ? options.content : folded(options.content);
    return haystack.find(needle) != std::string::npos;
}


bool excluded(GFile *location, const std::vector<GFile *> &roots)
{
    for (GFile *blocked : roots) {
        if (g_file_equal(location, blocked) || g_file_has_prefix(location, blocked))
            return true;
    }
    return false;
}

std::string digest(GFile *file, GCancellable *cancel, GError **error)
{
    GFileInputStream *stream = g_file_read(file, cancel, error);
    if (stream == nullptr) return {};
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    std::array<guint8, 65536> bytes{};
    guint64 total = 0;
    while (true) {
        const gssize got = g_input_stream_read(
            G_INPUT_STREAM(stream), bytes.data(), bytes.size(), cancel, error);
        if (got <= 0) break;
        total += static_cast<guint64>(got);
        if (total > 8U * 1024U * 1024U) break;
        g_checksum_update(sum, bytes.data(), static_cast<gsize>(got));
    }
    const std::string value = *error == nullptr && total <= 8U * 1024U * 1024U ?
        g_checksum_get_string(sum) : "";
    g_checksum_free(sum);
    g_input_stream_close(G_INPUT_STREAM(stream), nullptr, nullptr);
    g_object_unref(stream);
    return value;
}

void incomplete(SearchReport &report, const char *reason)
{
    report.complete = false;
    if (report.limitation.empty())
        report.limitation = reason != nullptr ? reason : "Provider unavailable";
}

struct Directory {
    GFile *folder{nullptr};
    GFileEnumerator *enumerator{nullptr};
    std::size_t depth{0};
};

void release(Directory &dir)
{
    if (dir.enumerator != nullptr) {
        g_file_enumerator_close(dir.enumerator, nullptr, nullptr);
        g_object_unref(dir.enumerator);
    }
    if (dir.folder != nullptr) g_object_unref(dir.folder);
}

} // namespace

SearchReport search_files(GFile *root, const SearchOptions &o,
                          GCancellable *cancel, GError **error)
{
    SearchReport report;
    if (root == nullptr || cancel == nullptr || error == nullptr || *error != nullptr) {
        if (error != nullptr && *error == nullptr)
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Search requires a directory and cancellable");
        return report;
    }
    if (!g_utf8_validate(o.name.c_str(), -1, nullptr) ||
        !g_utf8_validate(o.content.c_str(), -1, nullptr) ||
        (o.minimum_size && o.maximum_size && *o.minimum_size > *o.maximum_size) ||
        (o.modified_since && *o.modified_since < 0) ||
        (o.modified_before && *o.modified_before < 0) ||
        (o.modified_since && o.modified_before &&
         *o.modified_since >= *o.modified_before) ||
        o.excluded_uris.size() > 64 || o.tagged_uris.size() > 512) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Invalid search pattern, text or size bounds");
        return report;
    }
    const std::set<std::string> tagged(o.tagged_uris.begin(), o.tagged_uris.end());
    std::vector<GFile *> blocked;
    for (const std::string &uri : o.excluded_uris) {
        GFile *file = g_file_new_for_uri(uri.c_str());
        char *scheme = g_uri_parse_scheme(uri.c_str());
        const bool valid_scheme = scheme != nullptr;
        g_free(scheme);
        if (uri.empty() || !valid_scheme || file == nullptr) {
            for (GFile *object : blocked) g_object_unref(object);
            if (file != nullptr) g_object_unref(file);
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Excluded locations must be valid absolute URIs");
            return report;
        }
        blocked.push_back(file);
    }
    if (excluded(root, blocked)) {
        report.complete = false;
        report.limitation = "The search root was explicitly excluded";
        for (GFile *object : blocked) g_object_unref(object);
        return report;
    }
    GFileInfo *root_info = g_file_query_info(root, kAttributes,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, error);
    if (root_info == nullptr) {
        for (GFile *object : blocked) g_object_unref(object);
        return report;
    }
    if (g_file_info_get_file_type(root_info) != G_FILE_TYPE_DIRECTORY) {
        g_object_unref(root_info);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY, "Search root is not a directory");
        for (GFile *object : blocked) g_object_unref(object);
        return report;
    }
    const bool have_device = g_file_info_has_attribute(root_info, G_FILE_ATTRIBUTE_UNIX_DEVICE);
    const guint32 root_device = g_file_info_get_attribute_uint32(root_info, G_FILE_ATTRIBUTE_UNIX_DEVICE);
    g_object_unref(root_info);

    std::vector<Directory> stack;
    // Retain bounded candidate records only. The digest never becomes an
    // authoritative identity or proof of byte-exact equivalence.
    std::vector<std::pair<std::string, SearchHit>> duplicate_candidates;
    auto push = [&](GFile *folder, std::size_t depth, bool root_folder) {
        GError *local = nullptr;
        GFileEnumerator *enumerator = g_file_enumerate_children(folder, kAttributes,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, &local);
        if (enumerator == nullptr) {
            if (root_folder || g_error_matches(local, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                g_propagate_error(error, local);
            } else {
                incomplete(report, local != nullptr ? local->message : "Could not list subfolder");
                g_clear_error(&local);
            }
            return false;
        }
        stack.push_back(Directory{G_FILE(g_object_ref(folder)), enumerator, depth});
        return true;
    };
    if (!push(root, 0, true)) {
        for (GFile *object : blocked) g_object_unref(object);
        return report;
    }
    while (!stack.empty() && *error == nullptr) {
        if (g_cancellable_set_error_if_cancelled(cancel, error)) break;
        Directory &current = stack.back();
        GError *local = nullptr;
        GFileInfo *info = g_file_enumerator_next_file(current.enumerator, cancel, &local);
        if (info == nullptr) {
            if (local != nullptr) {
                if (g_error_matches(local, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                    g_propagate_error(error, local);
                else {
                    incomplete(report, local->message);
                    g_clear_error(&local);
                }
            }
            release(current);
            stack.pop_back();
            continue;
        }
        ++report.visited;
        if (report.visited > kMaxVisited) {
            incomplete(report, "Search stopped at 100,000 entries");
            g_object_unref(info);
            break;
        }
        const char *raw_name = g_file_info_get_name(info);
        if (raw_name == nullptr || *raw_name == '\0') {
            incomplete(report, "A provider returned an entry without a filename");
            g_object_unref(info);
            continue;
        }
        if (!o.include_hidden && g_file_info_get_is_hidden(info)) {
            g_object_unref(info);
            continue;
        }
        const GFileType type = g_file_info_get_file_type(info);
        const bool directory = type == G_FILE_TYPE_DIRECTORY;
        const bool regular = type == G_FILE_TYPE_REGULAR;
        GFile *child = g_file_get_child(current.folder, raw_name);
        if (excluded(child, blocked)) {
            g_object_unref(child);
            g_object_unref(info);
            continue;
        }
        const char *shown = g_file_info_get_display_name(info);
        const std::string name = shown != nullptr ? shown : raw_name;
        const goffset signed_size = g_file_info_get_size(info);
        const bool size_known = g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE) &&
                                signed_size >= 0;
        const std::uint64_t size = size_known ? static_cast<std::uint64_t>(signed_size) : 0;
        bool matching = matches_name(name, o);
        if (o.kind == SearchItemKind::RegularFiles && !regular) matching = false;
        if (o.kind == SearchItemKind::Directories && !directory) matching = false;
        if (o.minimum_size || o.maximum_size) {
            if (!size_known || (o.minimum_size && size < *o.minimum_size) ||
                (o.maximum_size && size > *o.maximum_size))
                matching = false;
        }
        if (o.modified_since || o.modified_before) {
            if (!g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_MODIFIED)) {
                matching = false;
            } else {
                const guint64 when =
                    g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
                if ((o.modified_since &&
                     when < static_cast<guint64>(*o.modified_since)) ||
                    (o.modified_before &&
                     when >= static_cast<guint64>(*o.modified_before)))
                    matching = false;
            }
        }
        if (o.owner_uid &&
            (!g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_UID) ||
             g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_UID) != *o.owner_uid))
            matching = false;
        if (o.restrict_tagged_uris) {
            char *uri = g_file_get_uri(child);
            if (uri == nullptr || !tagged.contains(uri)) matching = false;
            g_free(uri);
        }
        if (o.duplicates_by_sha256) {
            if (!regular) {
                // Directories/symlinks are deliberately not candidates and
                // cannot make a complete regular-file duplicate search partial.
                matching = false;
            } else if (!size_known || size > 8U * 1024U * 1024U) {
                if (matching) ++report.duplicate_skipped;
                matching = false;
            }
        }
        if (matching && !o.content.empty()) {
            const char *mime = g_file_info_get_content_type(info);
            const bool text = regular && mime != nullptr &&
                              g_content_type_is_a(mime, "text/plain");
            if (!text || !size_known || signed_size > kMaxTextBytes) {
                ++report.content_skipped;
                matching = false;
            } else {
                GError *read_error = nullptr;
                matching = includes_text(child, o, cancel, report, &read_error);
                if (read_error != nullptr) {
                    if (g_error_matches(read_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        g_propagate_error(error, read_error);
                    else {
                        incomplete(report, read_error->message);
                        g_clear_error(&read_error);
                    }
                    matching = false;
                }
            }
        }
        std::string hash;
        if (matching && o.duplicates_by_sha256 && *error == nullptr) {
            GError *failure = nullptr;
            hash = digest(child, cancel, &failure);
            if (failure != nullptr) {
                if (g_error_matches(failure, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                    g_propagate_error(error, failure);
                else {
                    incomplete(report, failure->message);
                    g_clear_error(&failure);
                }
                matching = false;
            } else if (hash.empty()) {
                ++report.duplicate_skipped;
                matching = false;
            }
        }
        if (matching && *error == nullptr) {
            char *uri = g_file_get_uri(child);
            char *path = g_file_get_parse_name(child);
            SearchHit hit{uri != nullptr ? uri : "",
                name, path != nullptr ? path : "", directory, size};
            if (o.duplicates_by_sha256) duplicate_candidates.emplace_back(hash, std::move(hit));
            else report.hits.push_back(std::move(hit));
            g_free(uri);
            g_free(path);
        }
        if (o.recursive && directory && *error == nullptr) {
            const bool other_device = have_device &&
                g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_DEVICE) &&
                g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_DEVICE) != root_device;
            if (!other_device) {
                if (current.depth + 1 >= kMaxDepth) {
                    incomplete(report, "Search stopped at the 64-level directory nesting limit");
                } else {
                    (void)push(child, current.depth + 1, false);
                }
            }
        }
        g_object_unref(child);
        g_object_unref(info);
        if ((o.duplicates_by_sha256 ? duplicate_candidates.size() : report.hits.size())
            >= kMaxResults) {
            incomplete(report, "Search stopped at 500 matching candidates");
            break;
        }
    }
    for (Directory &dir : stack) release(dir);
    for (GFile *object : blocked) g_object_unref(object);
    if (o.duplicates_by_sha256 && *error == nullptr) {
        std::map<std::pair<std::uint64_t, std::string>, std::size_t> counts;
        for (const auto &candidate : duplicate_candidates)
            ++counts[{candidate.second.size, candidate.first}];
        for (auto &candidate : duplicate_candidates) {
            if (counts[{candidate.second.size, candidate.first}] > 1)
                report.hits.push_back(std::move(candidate.second));
        }
        if (report.duplicate_skipped != 0)
            incomplete(report, "Some files were not hashable within the 8 MiB limit");
    }
    return report;
}

} // namespace infiltrator::files
