// SPDX-License-Identifier: GPL-3.0-or-later
#include "tag_catalogue.hpp"
#include "presentation_store.hpp"

#include <gio/gio.h>
#include <algorithm>
#include <sstream>
#include <utility>

namespace infiltrator::files {
namespace {
constexpr int kMaximumEntries = 512;
constexpr int kMaximumLabelsPerItem = 16;
constexpr std::size_t kMaximumUriBytes = 8192;

bool valid_uri(const std::string &uri)
{
    if (uri.empty() || uri.size() > kMaximumUriBytes ||
        uri.find('\0') != std::string::npos ||
        !g_utf8_validate(uri.c_str(), -1, nullptr)) return false;
    char *scheme = g_uri_parse_scheme(uri.c_str());
    const bool valid = scheme != nullptr;
    g_free(scheme);
    return valid;
}

std::string tsv_field(const std::string &value)
{
    // RFC-4180-style quotes remain copy/paste friendly for tabs and newlines.
    std::string out = "\"";
    for (char ch : value) {
        if (ch == '"') out += "\"\"";
        else out += ch;
    }
    return out + "\"";
}
}

bool valid_tag(const std::string &name)
{
    if (name.empty() || name.size() > 64 ||
        !g_utf8_validate(name.c_str(), -1, nullptr)) return false;
    for (unsigned char c : name) {
        if (c < 0x20U || c == 0x7fU || c == ',' || c == ';') return false;
    }
    return true;
}

TagCatalogue load_tag_catalogue()
{
    TagCatalogue result;
    std::string contents, error;
    const auto outcome = presentation_store::read("tags.ini", contents, error);
    if (outcome == presentation_store::ReadStatus::Missing) return result;
    if (outcome == presentation_store::ReadStatus::Failed) {
        result.writable = false;
        result.error = error;
        return result;
    }
    GKeyFile *keys = g_key_file_new();
    GError *parse = nullptr;
    bool good = g_key_file_load_from_data(
        keys, contents.data(), contents.size(), G_KEY_FILE_NONE, &parse);
    int count = -1;
    if (good) {
        count = g_key_file_get_integer(keys, "Tags", "count", &parse);
        good = parse == nullptr && count >= 0 && count <= kMaximumEntries &&
               g_key_file_get_integer(keys, "Tags", "version", nullptr) == 1;
    }
    for (int i = 0; good && i < count; ++i) {
        const std::string group = "Item " + std::to_string(i);
        char *uri = g_key_file_get_string(keys, group.c_str(), "uri", &parse);
        gsize quantity = 0;
        gchar **labels = parse == nullptr ?
            g_key_file_get_string_list(keys, group.c_str(), "labels", &quantity, &parse)
            : nullptr;
        good = parse == nullptr && uri != nullptr && valid_uri(uri) &&
               quantity > 0 && quantity <= kMaximumLabelsPerItem;
        std::set<std::string> tagset;
        for (gsize j = 0; good && j < quantity; ++j) {
            good = labels[j] != nullptr && valid_tag(labels[j]) &&
                   tagset.insert(labels[j]).second;
        }
        if (good) good = result.items.emplace(uri, std::move(tagset)).second;
        g_free(uri);
        g_strfreev(labels);
    }
    g_clear_error(&parse);
    g_key_file_unref(keys);
    if (!good) {
        result.items.clear();
        result.writable = false;
        result.error = "Existing tags.ini is invalid; it has been preserved.";
    }
    return result;
}

bool save_tag_catalogue(const TagCatalogue &value, std::string &error)
{
    if (!value.writable || value.items.size() > kMaximumEntries) {
        error = "Tag catalogue is read-only or exceeds its 512-item bound.";
        return false;
    }
    GKeyFile *keys = g_key_file_new();
    g_key_file_set_integer(keys, "Tags", "version", 1);
    g_key_file_set_integer(keys, "Tags", "count",
                           static_cast<int>(value.items.size()));
    int index = 0;
    for (const auto &[uri, tags] : value.items) {
        if (!valid_uri(uri) || tags.empty() ||
            tags.size() > kMaximumLabelsPerItem) {
            error = "Cannot serialize invalid URI or tag labels.";
            g_key_file_unref(keys);
            return false;
        }
        const std::string group = "Item " + std::to_string(index++);
        g_key_file_set_string(keys, group.c_str(), "uri", uri.c_str());
        std::vector<const gchar *> names;
        for (const std::string &name : tags) {
            if (!valid_tag(name)) {
                error = "Cannot serialize an invalid tag.";
                g_key_file_unref(keys);
                return false;
            }
            names.push_back(name.c_str());
        }
        g_key_file_set_string_list(keys, group.c_str(), "labels",
                                   names.data(), names.size());
    }
    gsize length = 0;
    char *data = g_key_file_to_data(keys, &length, nullptr);
    const bool written = data != nullptr &&
        presentation_store::write("tags.ini", std::string_view(data, length), error);
    g_free(data);
    g_key_file_unref(keys);
    return written;
}

bool apply_tags(const std::vector<std::string> &uris,
                const std::set<std::string> &labels, std::string &error)
{
    if (uris.empty() || uris.size() > 128 ||
        labels.size() > kMaximumLabelsPerItem ||
        !std::all_of(labels.begin(), labels.end(), valid_tag)) {
        error = "Choose 1–128 files and at most 16 valid tags.";
        return false;
    }
    // Merge from durable authority for each edit rather than using a stale
    // per-window cache. Unreadable or malformed state strictly disables writes.
    TagCatalogue tags = load_tag_catalogue();
    if (!tags.writable) {
        error = tags.error;
        return false;
    }
    for (const std::string &uri : uris) {
        if (!valid_uri(uri)) {
            error = "A selected file's URI is invalid.";
            return false;
        }
        if (labels.empty()) tags.items.erase(uri);
        else tags.items[uri] = labels;
    }
    return save_tag_catalogue(tags, error);
}

std::set<std::string> tags_for(const TagCatalogue &tags, const std::string &uri)
{
    const auto it = tags.items.find(uri);
    return it == tags.items.end() ? std::set<std::string>{} : it->second;
}

std::vector<std::string> uris_for_tag(const TagCatalogue &tags, const std::string &label)
{
    std::vector<std::string> result;
    for (const auto &[uri, labels] : tags.items)
        if (labels.contains(label)) result.push_back(uri);
    return result;
}

std::string export_tags_tsv(const TagCatalogue &tags)
{
    std::string text = "File URI\tTag\n";
    for (const auto &[uri, labels] : tags.items)
        for (const std::string &label : labels)
            text += tsv_field(uri) + "\t" + tsv_field(label) + "\n";
    return text;
}


bool import_tags_tsv(std::string_view input, std::string &error)
{
    constexpr std::string_view header = "File URI\tTag\n";
    if (input.size() > 1048576U || !input.starts_with(header)) {
        error = "Tag TSV must have the File URI and Tag header and be at most 1 MiB.";
        return false;
    }
    TagCatalogue rebuilt;
    std::size_t pos = header.size();
    auto field = [&](std::string &output) -> bool {
        if (pos >= input.size() || input[pos++] != '"') return false;
        output.clear();
        while (pos < input.size()) {
            const char ch = input[pos++];
            if (ch == '"') {
                if (pos < input.size() && input[pos] == '"') {
                    output.push_back('"'); ++pos;
                } else return true;
            } else {
                if (ch == '\n' || ch == '\r' || ch == '\0') return false;
                output.push_back(ch);
            }
            if (output.size() > kMaximumUriBytes) return false;
        }
        return false;
    };
    while (pos < input.size()) {
        std::string uri, tag;
        if (!field(uri) || pos >= input.size() || input[pos++] != '\t' ||
            !field(tag) || pos >= input.size() || input[pos++] != '\n' ||
            !valid_uri(uri) || !valid_tag(tag)) {
            error = "Malformed tag TSV row; no existing catalogue was changed.";
            return false;
        }
        auto &labels = rebuilt.items[uri];
        if (!labels.insert(tag).second || labels.size() > kMaximumLabelsPerItem ||
            rebuilt.items.size() > kMaximumEntries) {
            error = "Duplicate tag or catalogue capacity exceeded; nothing imported.";
            return false;
        }
    }
    // Zero-tag imports are an explicit empty replacement. They may be used to
    // clear a healthy derived database but never to overwrite corrupt state.
    TagCatalogue existing = load_tag_catalogue();
    if (!existing.writable) {
        error = existing.error;
        return false;
    }
    return save_tag_catalogue(rebuilt, error);
}


} // namespace infiltrator::files
