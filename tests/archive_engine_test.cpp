// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/core/archive_engine.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace infiltrator::files;

static void sample_archive(const fs::path &filename, const char *entry_name)
{
    archive *writer = archive_write_new();
    if (!writer) throw std::runtime_error("Failed to initialise archive writer");
    archive_write_set_format_zip(writer);
    if (archive_write_open_filename(writer, filename.c_str()) != ARCHIVE_OK)
        throw std::runtime_error("Could not create ZIP fixture");
    archive_entry *entry = archive_entry_new();
    archive_entry_set_pathname(entry, entry_name);
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_size(entry, 5);
    if (archive_write_header(writer, entry) != ARCHIVE_OK ||
        archive_write_data(writer, "hello", 5) != 5)
        throw std::runtime_error("Could not write ZIP fixture");
    archive_entry_free(entry);
    archive_write_close(writer);
    archive_write_free(writer);
}

int main()
{
    char template_path[] = "/tmp/files-archive-test-XXXXXX";
    const char *raw = ::mkdtemp(template_path);
    if (!raw) return 1;
    const fs::path root(raw);
    const auto cleanup = [&] { std::error_code ec; fs::remove_all(root, ec); };
    try {
        const fs::path good = root / "safe.zip";
        sample_archive(good, "docs/note.txt");
        ArchiveEngine engine;
        const auto imported = engine.extract(good, root, "fixture-1");
        if (!imported.ok() || !fs::exists(root / "safe" / "docs" / "note.txt"))
            throw std::runtime_error("Safe archive extraction did not publish its contents: " + imported.message);
        const auto collision = engine.extract(good, root, "fixture-2");
        if (collision.ok()) throw std::runtime_error("Extraction overwrote an existing destination");

        const fs::path input = root / "payload.txt";
        { std::ofstream writer(input); writer << "hello"; }
        const auto zipped = engine.create_zip(input, root, "zip-1");
        if (!zipped.ok() || !fs::exists(root / "payload.txt.zip"))
            throw std::runtime_error("Safe ZIP creation failed: " + zipped.message);
        if (engine.create_zip(input, root, "zip-2").ok())
            throw std::runtime_error("ZIP creation silently overwrote an existing archive");
        fs::create_directory(root / "unpack");
        const auto unzipped = engine.extract(root / "payload.txt.zip", root / "unpack", "zip-3");
        if (!unzipped.ok() || !fs::exists(root / "unpack" / "payload.txt" / "payload.txt"))
            throw std::runtime_error("ZIP round-trip failed: " + unzipped.message);
        const fs::path evil = root / "escape.zip";
        sample_archive(evil, "../outside.txt");
        const auto refused = engine.extract(evil, root, "fixture-3");
        if (refused.ok() || fs::exists(root / "outside.txt"))
            throw std::runtime_error("Traversal archive escaped its private root");
        cleanup();
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << '\n';
        cleanup();
        return 1;
    }
}
