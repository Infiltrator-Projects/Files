// SPDX-License-Identifier: GPL-3.0-or-later
#include "search_persistence.hpp"
#include "presentation_store.hpp"
#include "tag_catalogue.hpp"

#include <gio/gio.h>
#include <algorithm>
#include <charconv>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace infiltrator::files {
namespace {
constexpr int kMaximumPresets = 16;
constexpr int kMaximumPrivacyEntries = 64;

bool absolute_uri(const std::string &uri)
{
    if (uri.empty() || uri.size() > 8192 ||
        !g_utf8_validate(uri.c_str(), -1, nullptr)) return false;
    char *scheme = g_uri_parse_scheme(uri.c_str());
    const bool valid = scheme != nullptr;
    g_free(scheme);
    return valid;
}

std::string get(GKeyFile *keys, const char *group, const char *field, bool &valid)
{
    GError *error = nullptr;
    char *raw = g_key_file_get_string(keys, group, field, &error);
    if (error != nullptr || raw == nullptr) valid = false;
    std::string value = raw != nullptr ? raw : "";
    g_free(raw);
    g_clear_error(&error);
    return value;
}

std::uint64_t unsigned_field(GKeyFile *keys, const char *group, const char *key, bool &valid)
{
    const std::string raw = get(keys, group, key, valid);
    std::uint64_t result = 0;
    if (!valid || raw.empty()) { valid = false; return 0; }
    const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != raw.data() + raw.size()) valid = false;
    return result;
}

std::int64_t signed_field(GKeyFile *keys, const char *group, const char *key, bool &valid)
{
    const std::string raw = get(keys, group, key, valid);
    std::int64_t result = 0;
    if (!valid || raw.empty()) { valid = false; return 0; }
    const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != raw.data() + raw.size()) valid = false;
    return result;
}

bool boolean_field(GKeyFile *keys, const char *group, const char *key, bool &valid)
{
    GError *error = nullptr;
    const gboolean value = g_key_file_get_boolean(keys, group, key, &error);
    if (error != nullptr) valid = false;
    g_clear_error(&error);
    return value;
}

std::optional<std::uint64_t> optional_size(
    GKeyFile *keys, const char *group, const char *key, bool &valid)
{
    if (!boolean_field(keys, group, (std::string(key) + "-present").c_str(), valid))
        return std::nullopt;
    return unsigned_field(keys, group, key, valid);
}

std::optional<std::int64_t> optional_time(
    GKeyFile *keys, const char *group, const char *key, bool &valid)
{
    if (!boolean_field(keys, group, (std::string(key) + "-present").c_str(), valid))
        return std::nullopt;
    return signed_field(keys, group, key, valid);
}

void set_string(GKeyFile *keys, const std::string &group,
                const char *key, const std::string &value)
{
    g_key_file_set_string(keys, group.c_str(), key, value.c_str());
}

void set_uint(GKeyFile *keys, const std::string &group,
              const char *key, std::uint64_t value)
{
    set_string(keys, group, key, std::to_string(value));
}

void set_int(GKeyFile *keys, const std::string &group,
             const char *key, std::int64_t value)
{
    set_string(keys, group, key, std::to_string(value));
}

void set_optional(GKeyFile *keys, const std::string &group,
                  const char *key, std::optional<std::uint64_t> value)
{
    g_key_file_set_boolean(keys, group.c_str(),
        (std::string(key) + "-present").c_str(), value.has_value());
    if (value) set_uint(keys, group, key, *value);
}

void set_optional_time(GKeyFile *keys, const std::string &group,
                       const char *key, std::optional<std::int64_t> value)
{
    g_key_file_set_boolean(keys, group.c_str(),
        (std::string(key) + "-present").c_str(), value.has_value());
    if (value) set_int(keys, group, key, *value);
}
} // namespace

SavedSearchList load_saved_searches()
{
    SavedSearchList result;
    std::string contents, error;
    auto status = presentation_store::read("saved-searches.ini", contents, error);
    if (status == presentation_store::ReadStatus::Missing) return result;
    if (status == presentation_store::ReadStatus::Failed) {
        result.writable = false; result.error = error; return result;
    }
    GKeyFile *keys = g_key_file_new();
    GError *parse = nullptr;
    bool valid = g_key_file_load_from_data(keys, contents.data(), contents.size(),
                                           G_KEY_FILE_NONE, &parse);
    int count = -1;
    if (valid) {
        count = g_key_file_get_integer(keys, "Saved Searches", "count", &parse);
        valid = parse == nullptr && count >= 0 && count <= kMaximumPresets &&
                g_key_file_get_integer(keys, "Saved Searches", "version", nullptr) == 1;
    }
    for (int i = 0; valid && i < count; ++i) {
        const std::string group = "Preset " + std::to_string(i);
        SavedSearch entry;
        entry.title = get(keys, group.c_str(), "title", valid);
        entry.root_uri = get(keys, group.c_str(), "root", valid);
        entry.tag = get(keys, group.c_str(), "tag", valid);
        auto &o = entry.options;
        o.name = get(keys, group.c_str(), "name", valid);
        o.content = get(keys, group.c_str(), "content", valid);
        o.case_sensitive = boolean_field(keys, group.c_str(), "case", valid);
        o.recursive = boolean_field(keys, group.c_str(), "recursive", valid);
        o.include_hidden = boolean_field(keys, group.c_str(), "hidden", valid);
        o.duplicates_by_sha256 = boolean_field(keys, group.c_str(), "duplicates", valid);
        const auto mode = unsigned_field(keys, group.c_str(), "mode", valid);
        const auto kind = unsigned_field(keys, group.c_str(), "kind", valid);
        o.minimum_size = optional_size(keys, group.c_str(), "minimum", valid);
        o.maximum_size = optional_size(keys, group.c_str(), "maximum", valid);
        o.modified_since = optional_time(keys, group.c_str(), "since", valid);
        o.modified_before = optional_time(keys, group.c_str(), "before", valid);
        auto owner = optional_size(keys, group.c_str(), "owner", valid);
        if (owner && *owner <= G_MAXUINT32) o.owner_uid = static_cast<std::uint32_t>(*owner);
        else if (owner) valid = false;
        valid = valid && !entry.title.empty() && entry.title.size() <= 80 &&
            absolute_uri(entry.root_uri) &&
            (entry.tag.empty() || valid_tag(entry.tag)) &&
            g_utf8_validate(o.name.c_str(), -1, nullptr) &&
            g_utf8_validate(o.content.c_str(), -1, nullptr) &&
            o.name.size() <= 512 && o.content.size() <= 512 &&
            mode <= 2 && kind <= 2 &&
            (!o.minimum_size || !o.maximum_size || *o.minimum_size <= *o.maximum_size) &&
            (!o.modified_since || !o.modified_before || *o.modified_since < *o.modified_before);
        for (const auto &prior : result.items)
            if (prior.title == entry.title) valid = false;
        if (valid) {
            o.name_mode = static_cast<SearchNameMode>(mode);
            o.kind = static_cast<SearchItemKind>(kind);
            result.items.push_back(std::move(entry));
        }
    }
    g_clear_error(&parse);
    g_key_file_unref(keys);
    if (!valid) {
        result.items.clear();
        result.writable = false;
        result.error = "Saved search catalogue is malformed; original bytes preserved.";
    }
    return result;
}

bool save_saved_searches(const SavedSearchList &value, std::string &error)
{
    if (!value.writable || value.items.size() > kMaximumPresets) {
        error = "Saved searches cannot be overwritten or exceed 16 presets.";
        return false;
    }
    GKeyFile *keys = g_key_file_new();
    g_key_file_set_integer(keys, "Saved Searches", "version", 1);
    g_key_file_set_integer(keys, "Saved Searches", "count",
                           static_cast<int>(value.items.size()));
    for (std::size_t i = 0; i < value.items.size(); ++i) {
        const auto &p = value.items[i];
        const std::string group = "Preset " + std::to_string(i);
        if (p.title.empty() || p.title.size() > 80 ||
            !absolute_uri(p.root_uri) || (!p.tag.empty() && !valid_tag(p.tag)) ||
            p.options.name.size() > 512 || p.options.content.size() > 512) {
            error = "Invalid saved-search parameters.";
            g_key_file_unref(keys);
            return false;
        }
        set_string(keys, group, "title", p.title);
        set_string(keys, group, "root", p.root_uri);
        set_string(keys, group, "tag", p.tag);
        set_string(keys, group, "name", p.options.name);
        set_string(keys, group, "content", p.options.content);
        g_key_file_set_boolean(keys, group.c_str(), "case", p.options.case_sensitive);
        g_key_file_set_boolean(keys, group.c_str(), "recursive", p.options.recursive);
        g_key_file_set_boolean(keys, group.c_str(), "hidden", p.options.include_hidden);
        g_key_file_set_boolean(keys, group.c_str(), "duplicates", p.options.duplicates_by_sha256);
        set_uint(keys, group, "mode", static_cast<std::uint64_t>(p.options.name_mode));
        set_uint(keys, group, "kind", static_cast<std::uint64_t>(p.options.kind));
        set_optional(keys, group, "minimum", p.options.minimum_size);
        set_optional(keys, group, "maximum", p.options.maximum_size);
        set_optional_time(keys, group, "since", p.options.modified_since);
        set_optional_time(keys, group, "before", p.options.modified_before);
        set_optional(keys, group, "owner", p.options.owner_uid ?
            std::optional<std::uint64_t>(*p.options.owner_uid) : std::nullopt);
    }
    gsize size = 0;
    char *text = g_key_file_to_data(keys, &size, nullptr);
    const bool result = text != nullptr &&
        presentation_store::write("saved-searches.ini", {text, size}, error);
    g_free(text);
    g_key_file_unref(keys);
    return result;
}

SearchPrivacy load_search_privacy()
{
    SearchPrivacy state;
    std::string contents, error;
    const auto result = presentation_store::read("search-private.ini", contents, error);
    if (result == presentation_store::ReadStatus::Missing) return state;
    if (result == presentation_store::ReadStatus::Failed) {
        state.valid = false; state.error = error; return state;
    }
    GKeyFile *keys = g_key_file_new();
    GError *parse = nullptr;
    bool valid = g_key_file_load_from_data(keys, contents.data(), contents.size(),
                                           G_KEY_FILE_NONE, &parse);
    int count = -1;
    if (valid) {
        count = g_key_file_get_integer(keys, "Privacy", "count", &parse);
        valid = parse == nullptr && count >= 0 && count <= kMaximumPrivacyEntries &&
                g_key_file_get_integer(keys, "Privacy", "version", nullptr) == 1;
    }
    for (int i = 0; valid && i < count; ++i) {
        const std::string field = "uri-" + std::to_string(i);
        const std::string uri = get(keys, "Privacy", field.c_str(), valid);
        valid = valid && absolute_uri(uri) &&
            std::find(state.excluded_uris.begin(), state.excluded_uris.end(), uri) ==
                state.excluded_uris.end();
        if (valid) state.excluded_uris.push_back(uri);
    }
    g_clear_error(&parse);
    g_key_file_unref(keys);
    if (!valid) {
        state.valid = false;
        state.excluded_uris.clear();
        state.error = "Private search exclusions are malformed; search is disabled.";
    }
    return state;
}

// Activity tracking shares the search privacy authority. Invalid privacy
// state excludes *every* object: no private path can enter Recent on error.
bool privacy_allows_recent_uri(const SearchPrivacy &privacy, const std::string &uri)
{
    if (!privacy.valid || uri.empty()) return false;
    GFile *candidate = g_file_new_for_uri(uri.c_str());
    bool allowed = true;
    for (const std::string &excluded : privacy.excluded_uris) {
        GFile *root = g_file_new_for_uri(excluded.c_str());
        if (g_file_equal(candidate, root) || g_file_has_prefix(candidate, root))
            allowed = false;
        g_object_unref(root);
        if (!allowed) break;
    }
    g_object_unref(candidate);
    return allowed;
}

bool save_search_privacy(const std::vector<std::string> &uris, std::string &error)
{
    // Fail closed rather than replacing a malformed exclusion file and
    // inadvertently searching locations the user previously marked private.
    const auto previous = load_search_privacy();
    if (!previous.valid || uris.size() > kMaximumPrivacyEntries) {
        error = previous.valid ? "Privacy exclusions exceed 64 locations" : previous.error;
        return false;
    }
    GKeyFile *keys = g_key_file_new();
    g_key_file_set_integer(keys, "Privacy", "version", 1);
    g_key_file_set_integer(keys, "Privacy", "count", static_cast<int>(uris.size()));
    for (std::size_t i = 0; i < uris.size(); ++i) {
        if (!absolute_uri(uris[i]) ||
            std::find(uris.begin(), uris.begin() + static_cast<std::ptrdiff_t>(i),
                      uris[i]) != uris.begin() + static_cast<std::ptrdiff_t>(i)) {
            error = "Private exclusions must contain distinct absolute URIs.";
            g_key_file_unref(keys);
            return false;
        }
        const std::string key = "uri-" + std::to_string(i);
        g_key_file_set_string(keys, "Privacy", key.c_str(), uris[i].c_str());
    }
    gsize bytes = 0;
    char *raw = g_key_file_to_data(keys, &bytes, nullptr);
    const bool saved = raw != nullptr &&
        presentation_store::write("search-private.ini", {raw, bytes}, error);
    g_free(raw);
    g_key_file_unref(keys);
    return saved;
}

} // namespace infiltrator::files
