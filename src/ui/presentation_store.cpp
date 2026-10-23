// SPDX-License-Identifier: GPL-3.0-or-later
#include "presentation_store.hpp"
#include <infiltratr/posix.h>

#include <cstdlib>
#include <system_error>
#include <vector>

namespace infiltrator::files::presentation_store {
namespace {
constexpr std::size_t limit = 1024U * 1024U;
std::string directory(std::string &error)
{
    char *config = nullptr;
    if (!infiltratr_xdg_config_home_alloc(&config)) {
        error = "The configuration directory is unavailable.";
        return {};
    }
    std::string path = std::string(config) + "/infiltrator/files";
    std::free(config);
    return path;
}
}
ReadStatus read(const char *name, std::string &contents, std::string &error)
{
    contents.clear(); error.clear();
    const auto folder = directory(error);
    if (folder.empty()) return ReadStatus::Failed;
    std::vector<char> buffer(limit + 1U);
    std::size_t bytes = 0U;
    const auto result = infiltratr_read_text_file_ex((folder + "/" + name).c_str(),
                                                    buffer.data(), buffer.size(), &bytes);
    if (result == INFILTRATR_IO_NOT_FOUND) return ReadStatus::Missing;
    if (result != INFILTRATR_IO_OK) {
        error = std::string("Saved presentation state could not be read: ") + infiltratr_io_result_name(result);
        return ReadStatus::Failed;
    }
    contents.assign(buffer.data(), bytes);
    return ReadStatus::Loaded;
}
bool write(const char *name, const std::string_view contents, std::string &error)
{
    error.clear();
    if (contents.size() > limit) {
        error = "The presentation state exceeds the 1 MiB storage limit.";
        return false;
    }
    const auto folder = directory(error);
    if (folder.empty()) return false;
    int result = infiltratr_mkdir_parents(folder.c_str(), 0700);
    if (result == 0)
        result = infiltratr_atomic_file_write_bytes((folder + "/" + name).c_str(),
            INFILTRATR_ATOMIC_FILE_PRIVATE, contents.data(), contents.size());
    if (result != 0) {
        error = "Presentation state could not be saved: " + std::generic_category().message(result);
        return false;
    }
    return true;
}
} // namespace infiltrator::files::presentation_store
