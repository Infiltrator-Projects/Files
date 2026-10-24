// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/core/provider_import_engine.hpp"
#include "../src/core/operation_journal.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

using namespace infiltrator::files;
namespace fs = std::filesystem;
int main()
{
    char pattern[] = "/tmp/files-provider-import-XXXXXX";
    char *root_path = ::mkdtemp(pattern);
    if (root_path == nullptr) return 1;
    const fs::path root(root_path);
    const auto cleanup = [&] { std::error_code ignored; fs::remove_all(root, ignored); };
    try {
        fs::create_directories(root / "source");
        fs::create_directories(root / "target");
        const fs::path input = root / "source" / "payload.txt";
        { std::ofstream writer(input, std::ios::binary); writer << "Provider import bytes\n"; }
        GFile *source = g_file_new_for_path(input.c_str());
        char *uri = g_file_get_uri(source);
        g_object_unref(source);
        ProviderImportEngine engine;
        const auto result = engine.import_regular_file(uri, root / "target", "test-1");
        if (!result.ok()) throw std::runtime_error(result.message);
        if (!fs::exists(root / "target" / "payload.txt"))
            throw std::runtime_error("Verified import missing");
        const auto repeat = engine.import_regular_file(uri, root / "target", "test-2");
        if (repeat.ok()) throw std::runtime_error("Existing destination must not be replaced");
        const auto folder = engine.import_regular_file(uri, root / "target", "bad-slash/1");
        if (folder.ok()) throw std::runtime_error("Invalid journal ID accepted");
        const fs::path large = root / "source" / "too-large.bin";
        { std::ofstream file(large); file << "x"; }
        fs::resize_file(large, 1024ULL * 1024ULL * 1024ULL + 1ULL);
        GFile *large_file = g_file_new_for_path(large.c_str());
        char *large_uri = g_file_get_uri(large_file);
        const auto too_large = engine.import_regular_file(large_uri, root / "target", "size-limit");
        if (too_large.status != OperationStatus::UnsupportedCapability ||
            fs::exists(root / "target" / "too-large.bin"))
            throw std::runtime_error("Oversized provider import was not refused");
        g_free(large_uri); g_object_unref(large_file);
        fs::create_symlink(input, root / "source" / "linked");
        GFile *link = g_file_new_for_path((root / "source" / "linked").c_str());
        char *link_uri = g_file_get_uri(link);
        const auto denied = engine.import_regular_file(link_uri, root / "target", "test-3");
        if (denied.ok()) throw std::runtime_error("Provider symlink must be refused");
        g_free(link_uri); g_object_unref(link);
        g_free(uri);
        cleanup(); return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        cleanup(); return 1;
    }
}
