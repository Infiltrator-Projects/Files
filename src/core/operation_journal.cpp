// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_journal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <vector>

namespace infiltrator::files {

namespace {

std::atomic_uint64_t sequence{0};

std::string status_name(const OperationStatus status)
{
    switch (status) {
    case OperationStatus::Success:
        return "success";
    case OperationStatus::InvalidRequest:
        return "invalid-request";
    case OperationStatus::DestinationConflict:
        return "destination-conflict";
    case OperationStatus::PermissionFailure:
        return "permission-failure";
    case OperationStatus::ReadOnlyLocation:
        return "read-only-location";
    case OperationStatus::UnsupportedCapability:
        return "unsupported-capability";
    case OperationStatus::Cancelled:
        return "cancelled";
    case OperationStatus::ExecutionFailure:
        return "execution-failure";
    case OperationStatus::VerificationFailure:
        return "verification-failure";
    }
    return "unknown";
}

std::uint64_t epoch_millis()
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

std::vector<std::string> split_tabs(const std::string &line)
{
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (start <= line.size()) {
        const std::size_t separator = line.find('\t', start);
        if (separator == std::string::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, separator - start));
        start = separator + 1U;
    }
    return fields;
}

} // namespace

OperationJournal::OperationJournal(std::filesystem::path path) : path_(std::move(path)) {}

std::filesystem::path OperationJournal::default_path()
{
    if (const char *state_home = std::getenv("XDG_STATE_HOME");
        state_home != nullptr && state_home[0] != '\0') {
        return std::filesystem::path(state_home) / "infiltrator-file-manager" / "operations.log";
    }
    if (const char *home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home) / ".local" / "state" /
               "infiltrator-file-manager" / "operations.log";
    }
    return std::filesystem::temp_directory_path() / "infiltrator-file-manager-operations.log";
}

std::string OperationJournal::escape(const std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        switch (character) {
        case '\\':
            escaped += "\\\\";
            break;
        case '\t':
            escaped += "\\t";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        default:
            escaped += character;
            break;
        }
    }
    return escaped;
}

std::string OperationJournal::unescape(const std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    bool escaped = false;
    for (const char character : value) {
        if (!escaped) {
            if (character == '\\') {
                escaped = true;
            } else {
                result += character;
            }
            continue;
        }

        switch (character) {
        case '\\':
            result += '\\';
            break;
        case 't':
            result += '\t';
            break;
        case 'n':
            result += '\n';
            break;
        case 'r':
            result += '\r';
            break;
        default:
            result += '\\';
            result += character;
            break;
        }
        escaped = false;
    }
    if (escaped) {
        result += '\\';
    }
    return result;
}

bool OperationJournal::append_line(const std::string &line) const
{
    std::error_code error;
    const auto parent = path_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return false;
        }
    }

    const int descriptor = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return false;
    }

    std::string record = line;
    record.push_back('\n');
    std::size_t written = 0;
    bool ok = true;
    while (written < record.size()) {
        const ssize_t count = ::write(descriptor,
                                      record.data() + written,
                                      record.size() - written);
        if (count <= 0) {
            ok = false;
            break;
        }
        written += static_cast<std::size_t>(count);
    }
    if (ok && ::fsync(descriptor) != 0) {
        ok = false;
    }
    if (::close(descriptor) != 0) {
        ok = false;
    }
    return ok;
}

std::string OperationJournal::begin(const std::string_view kind,
                                    const std::string_view source,
                                    const std::string_view destination) const
{
    const std::uint64_t now = epoch_millis();
    const std::uint64_t serial = sequence.fetch_add(1, std::memory_order_relaxed) + 1U;
    std::ostringstream id;
    id << now << '-' << static_cast<unsigned long>(::getpid()) << '-' << serial;

    std::ostringstream line;
    line << now << '\t' << id.str() << "\tSTART\t" << escape(kind) << '\t'
         << escape(source) << '\t' << escape(destination);
    if (!append_line(line.str())) {
        return {};
    }
    return id.str();
}

bool OperationJournal::finish(const std::string_view operation_id,
                              const std::string_view kind,
                              const OperationResult &result) const
{
    if (operation_id.empty()) {
        return false;
    }
    const std::uint64_t now = epoch_millis();
    std::ostringstream line;
    line << now << '\t' << escape(operation_id) << "\tEND\t" << escape(kind) << '\t'
         << status_name(result.status) << '\t' << (result.changed ? "changed" : "unchanged")
         << '\t' << escape(result.destination.string()) << '\t' << escape(result.message);
    return append_line(line.str());
}

std::vector<InterruptedOperation> OperationJournal::interrupted_operations() const
{
    std::ifstream stream(path_, std::ios::binary);
    if (!stream) {
        return {};
    }

    std::unordered_map<std::string, InterruptedOperation> open;
    std::string line;
    while (std::getline(stream, line)) {
        const std::vector<std::string> fields = split_tabs(line);
        if (fields.size() < 4U) {
            continue;
        }

        const std::string id = unescape(fields[1]);
        if (id.empty()) {
            continue;
        }
        if (fields[2] == "END") {
            open.erase(id);
            continue;
        }
        if (fields[2] != "START" || fields.size() < 6U) {
            continue;
        }

        InterruptedOperation operation;
        try {
            operation.started_millis = static_cast<std::uint64_t>(std::stoull(fields[0]));
        } catch (...) {
            continue;
        }
        operation.operation_id = id;
        operation.kind = unescape(fields[3]);
        operation.source = unescape(fields[4]);
        operation.destination = unescape(fields[5]);
        open[id] = std::move(operation);
    }

    std::vector<InterruptedOperation> interrupted;
    interrupted.reserve(open.size());
    for (auto &entry : open) {
        interrupted.push_back(std::move(entry.second));
    }
    std::sort(interrupted.begin(), interrupted.end(),
              [](const InterruptedOperation &left, const InterruptedOperation &right) {
                  return left.started_millis < right.started_millis;
              });
    return interrupted;
}

bool OperationJournal::mark_interrupted(const InterruptedOperation &operation) const
{
    if (operation.operation_id.empty()) {
        return false;
    }
    const OperationResult result{
        OperationStatus::VerificationFailure,
        OperationPhase::Verify,
        operation.destination,
        "The previous Files session ended before this operation recorded completion. Inspect the source and destination before assuming either state is authoritative.",
        true,
    };
    return finish(operation.operation_id, operation.kind, result);
}

} // namespace infiltrator::files
