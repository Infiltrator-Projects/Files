// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_journal.hpp"

#include <infiltratr/posix.h>
#include <infiltratr/posix_io.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <csignal>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <climits>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <vector>

namespace infiltrator::files {

namespace {

std::atomic_uint64_t sequence{0};
std::mutex journal_mutex;
// Prevent an unbounded log from exhausting memory or hiding recovery state.
// Oversized journals fail closed until explicitly inspected/archived.
constexpr std::uintmax_t kMaximumJournalBytes = 128U * 1024U * 1024U;

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

const std::string &current_session_id()
{
    static const std::string id = [] {
        using namespace std::chrono;
        const auto wall = static_cast<std::uint64_t>(
            duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
        const auto monotonic = static_cast<std::uint64_t>(
            duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
        std::ostringstream value;
        value << std::hex << wall << '-' << static_cast<unsigned long>(::getpid())
              << '-' << monotonic;
        return value.str();
    }();
    return id;
}

bool belongs_to_current_session(const std::string_view operation_id)
{
    const std::string &session = current_session_id();
    return operation_id.size() > session.size() &&
           operation_id.compare(0U, session.size(), session) == 0 &&
           operation_id[session.size()] == '-';
}

// Session IDs contain a hexadecimal creation time, hexadecimal PID,
// monotonic nonce and operation serial. Do not call an operation "crashed"
// while its owner process is still running in another Files window/process.
// PID reuse intentionally errs toward retaining evidence rather than
// falsely closing a live operation.
bool belongs_to_live_process(const std::string_view id)
{
    if (belongs_to_current_session(id)) return true;
    const std::size_t first = id.find('-');
    if (first == std::string_view::npos) return false;
    const std::size_t second = id.find('-', first + 1U);
    if (second == std::string_view::npos ||
        id.find('-', second + 1U) == std::string_view::npos) return false;
    const auto pid_text = id.substr(first + 1U, second - first - 1U);
    unsigned long long value = 0U;
    const auto parsed = std::from_chars(pid_text.data(),
                                        pid_text.data() + pid_text.size(), value, 16);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != pid_text.data() + pid_text.size() ||
        value == 0U || value > static_cast<unsigned long long>(INT_MAX)) return false;
    const pid_t pid = static_cast<pid_t>(value);
    if (::kill(pid, 0) == 0) return true;
    return errno == EPERM;
}

bool valid_record_escape(const std::string_view value)
{
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\') continue;
        if (++i == value.size()) return false;
        if (value[i] != '\\' && value[i] != 'n' &&
            value[i] != 't' && value[i] != 'r') return false;
    }
    return true;
}

bool valid_record_time(const std::string_view value)
{
    std::uint64_t timestamp = 0U;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), timestamp);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
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

// Recovery inspection and mutation admission accept exactly the same frames.
bool valid_record(const std::vector<std::string> &fields)
{
    const bool start = fields.size() == 6U && fields[2] == "START";
    const bool end = fields.size() == 8U && fields[2] == "END";
    if ((!start && !end) || !valid_record_time(fields[0]) ||
        fields[1].empty() || fields[3].empty()) return false;
    for (std::size_t index = 1U; index < fields.size(); ++index)
        if (!valid_record_escape(fields[index])) return false;
    return !end || ((fields[5] == "changed" || fields[5] == "unchanged") &&
                    !fields[4].empty());
}

// One bounded checkpoint avoids rescanning a growing journal for every batch
// item. It is trusted only under journal_mutex + flock and an exact inode,
// size, mtime and ctime match. External append, replacement or in-place edits
// invalidate it. No on-disk validation authority is introduced.
struct ValidatedJournal {
    struct stat status {};
    bool valid{false};
};
ValidatedJournal validated_journal;

bool same_journal_revision(const struct stat &left, const struct stat &right)
{
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
           left.st_size == right.st_size &&
           left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
           left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
           left.st_ctim.tv_sec == right.st_ctim.tv_sec &&
           left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
}

bool validate_existing_records(const int descriptor, const struct stat &status)
{
    if (validated_journal.valid &&
        same_journal_revision(validated_journal.status, status)) return true;
    validated_journal.valid = false;
    std::array<char, 4096> buffer{};
    std::string line;
    const auto size = static_cast<std::uint64_t>(status.st_size);
    for (std::uint64_t offset = 0U; offset < size;) {
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), size - offset));
        if (infiltratr_pread_full(descriptor, buffer.data(), count, offset) != 0)
            return false;
        for (std::size_t index = 0U; index < count; ++index) {
            if (buffer[index] == '\n') {
                if (!valid_record(split_tabs(line))) return false;
                line.clear();
            } else {
                line.push_back(buffer[index]);
            }
        }
        offset += count;
    }
    // The incomplete final frame is intentionally left to tail repair. A bad
    // complete frame must fail admission before any journal bytes are changed.
    return true;
}

bool lock_descriptor(const int descriptor, const int operation)
{
    int result = 0;
    do {
        result = ::flock(descriptor, operation);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

bool sync_descriptor(const int descriptor)
{
    int result = 0;
    do {
        result = ::fsync(descriptor);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

bool truncate_descriptor(const int descriptor, const off_t size)
{
    int result = 0;
    do {
        result = ::ftruncate(descriptor, size);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

bool discard_incomplete_tail(const int descriptor)
{
    struct stat status {};
    if (::fstat(descriptor, &status) != 0 || status.st_size < 0) {
        return false;
    }
    if (status.st_size == 0) {
        return true;
    }

    char final_byte = '\0';
    const auto final_offset = static_cast<std::uint64_t>(status.st_size - 1);
    if (infiltratr_pread_full(descriptor, &final_byte, 1U, final_offset) != 0) {
        return false;
    }
    if (final_byte == '\n') {
        return true;
    }

    // Only newline-terminated records are committed journal frames.  If a
    // process died during a write, remove that incomplete suffix before the
    // next append so two records can never be accidentally concatenated.
    std::array<char, 4096> buffer{};
    std::uint64_t cursor = static_cast<std::uint64_t>(status.st_size);
    while (cursor > 0U) {
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(cursor, buffer.size()));
        const std::uint64_t start = cursor - static_cast<std::uint64_t>(count);
        if (infiltratr_pread_full(descriptor, buffer.data(), count, start) != 0) {
            return false;
        }
        for (std::size_t index = count; index > 0U; --index) {
            if (buffer[index - 1U] == '\n') {
                const std::uint64_t retained = start + index;
                if (retained > static_cast<std::uint64_t>(
                                   std::numeric_limits<off_t>::max())) {
                    return false;
                }
                return truncate_descriptor(descriptor, static_cast<off_t>(retained));
            }
        }
        cursor = start;
    }
    return truncate_descriptor(descriptor, 0);
}

bool sync_parent_directory(const std::filesystem::path &path)
{
    const std::filesystem::path parent = path.parent_path();
    if (parent.empty()) {
        return true;
    }

    const int descriptor =
        ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        return false;
    }
    bool ok = sync_descriptor(descriptor);
    if (::close(descriptor) != 0) {
        ok = false;
    }
    return ok;
}

} // namespace

OperationJournal::OperationJournal(std::filesystem::path path) : path_(std::move(path)) {}

std::filesystem::path OperationJournal::default_path()
{
    if (const char *state_home = std::getenv("XDG_STATE_HOME");
        state_home != nullptr && state_home[0] != '\0') {
        const std::filesystem::path base(state_home);
        if (base.is_absolute()) {
            return base / "files" / "operations.log";
        }
    }

    if (const char *home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        const std::filesystem::path base(home);
        if (base.is_absolute()) {
            return base / ".local" / "state" /
                   "files" / "operations.log";
        }
    }

    // A durable operation journal has no honest temporary-directory fallback.
    // Returning an empty path makes begin() fail closed before user data mutates.
    return {};
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
    if (path_.empty() || !valid_record(split_tabs(line))) {
        return false;
    }

    std::lock_guard<std::mutex> process_lock(journal_mutex);
    const std::filesystem::path parent = path_.parent_path();
    if (!parent.empty()) {
        const int mkdir_error = infiltratr_mkdir_parents(parent.c_str(), 0700U);
        if (mkdir_error != 0) {
            return false;
        }
    }

    const int descriptor = ::open(path_.c_str(),
                                  O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW,
                                  0600);
    if (descriptor < 0) {
        return false;
    }

    const bool locked = lock_descriptor(descriptor, LOCK_EX);
    bool ok = locked;
    // Check the original size before tail repair. An oversized or corrupted
    // journal must not be silently truncated to zero by scanning for a newline.
    struct stat preflight_status {};
    if (ok && (::fstat(descriptor, &preflight_status) != 0 ||
               preflight_status.st_size < 0 ||
               static_cast<std::uintmax_t>(preflight_status.st_size) >
                   kMaximumJournalBytes)) {
        ok = false;
    }
    if (ok && (preflight_status.st_mode & 0777U) != 0600U &&
        ::fchmod(descriptor, 0600) != 0) ok = false;
    if (ok && !validate_existing_records(descriptor, preflight_status)) {
        ok = false;
    }
    if (ok && !discard_incomplete_tail(descriptor)) {
        ok = false;
    }

    std::string record = line;
    record.push_back('\n');
    struct stat journal_status {};
    if (ok && (::fstat(descriptor, &journal_status) != 0 ||
               journal_status.st_size < 0 ||
               static_cast<std::uintmax_t>(journal_status.st_size) >
                   kMaximumJournalBytes ||
               record.size() > kMaximumJournalBytes -
                   static_cast<std::uintmax_t>(journal_status.st_size))) {
        ok = false;
    }
    if (ok && infiltratr_write_full(descriptor, record.data(), record.size()) != 0) {
        ok = false;
    }
    if (ok && !sync_descriptor(descriptor)) {
        ok = false;
    }
    if (ok && !sync_parent_directory(path_)) {
        ok = false;
    }

    validated_journal.valid = ok &&
        ::fstat(descriptor, &validated_journal.status) == 0;

    if (locked && !lock_descriptor(descriptor, LOCK_UN)) {
        ok = false;
    }
    if (::close(descriptor) != 0) {
        ok = false;
    }
    return ok;
}

bool OperationJournal::read_snapshot(std::string &contents) const
{
    contents.clear();
    if (path_.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> process_lock(journal_mutex);
    const int descriptor = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        return errno == ENOENT;
    }

    const bool locked = lock_descriptor(descriptor, LOCK_SH);
    bool ok = locked;
    struct stat status {};
    if (ok && ::fstat(descriptor, &status) != 0) {
        ok = false;
    }
    if (ok && status.st_size < 0) {
        ok = false;
    }

    std::size_t size = 0U;
    if (ok) {
        const auto unsigned_size = static_cast<std::uintmax_t>(status.st_size);
        if (unsigned_size > kMaximumJournalBytes ||
            unsigned_size > static_cast<std::uintmax_t>(
                                std::numeric_limits<std::size_t>::max())) {
            ok = false;
        } else {
            size = static_cast<std::size_t>(unsigned_size);
            contents.resize(size);
        }
    }
    if (ok && size > 0U &&
        infiltratr_read_full(descriptor, contents.data(), size) != 0) {
        ok = false;
    }

    if (locked && !lock_descriptor(descriptor, LOCK_UN)) {
        ok = false;
    }
    if (::close(descriptor) != 0) {
        ok = false;
    }
    if (!ok) {
        contents.clear();
        return false;
    }

    // A crash may leave bytes after the final durable newline.  Such a record
    // has no complete framing and must never be promoted into recovery truth.
    if (!contents.empty() && contents.back() != '\n') {
        const std::size_t final_newline = contents.find_last_of('\n');
        if (final_newline == std::string::npos) {
            contents.clear();
        } else {
            contents.resize(final_newline + 1U);
        }
    }
    return true;
}

std::string OperationJournal::begin(const std::string_view kind,
                                    const std::string_view source,
                                    const std::string_view destination) const
{
    const std::uint64_t now = epoch_millis();
    const std::uint64_t serial = sequence.fetch_add(1, std::memory_order_relaxed) + 1U;
    const std::string operation_id = current_session_id() + '-' + std::to_string(serial);

    std::ostringstream line;
    line << now << '\t' << operation_id << "\tSTART\t" << escape(kind) << '\t'
         << escape(source) << '\t' << escape(destination);
    if (!append_line(line.str())) {
        return {};
    }
    return operation_id;
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

bool OperationJournal::interrupted_operations(
    std::vector<InterruptedOperation> &interrupted) const
{
    interrupted.clear();
    std::string contents;
    if (!read_snapshot(contents)) {
        return false;
    }
    if (contents.empty()) {
        return true;
    }

    std::unordered_map<std::string, InterruptedOperation> open;
    std::istringstream stream(contents);
    std::string line;
    while (std::getline(stream, line)) {
        const std::vector<std::string> fields = split_tabs(line);
        if (!valid_record(fields)) {
            interrupted.clear();
            return false;
        }
        const std::string id = unescape(fields[1]);
        if (fields[2] == "END") {
            open.erase(id);
            continue;
        }
        if (belongs_to_live_process(id)) continue;

        InterruptedOperation operation;
        const auto millis = std::string_view(fields[0]);
        const auto parsed = std::from_chars(
            millis.data(), millis.data() + millis.size(), operation.started_millis);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != millis.data() + millis.size()) {
            interrupted.clear();
            return false;
        }
        operation.operation_id = id;
        operation.kind = unescape(fields[3]);
        operation.source = unescape(fields[4]);
        operation.destination = unescape(fields[5]);
        open[id] = std::move(operation);
    }

    interrupted.reserve(open.size());
    for (auto &entry : open) {
        interrupted.push_back(std::move(entry.second));
    }
    std::sort(interrupted.begin(), interrupted.end(),
              [](const InterruptedOperation &left, const InterruptedOperation &right) {
                  return left.started_millis < right.started_millis;
              });
    return true;
}

std::vector<InterruptedOperation> OperationJournal::interrupted_operations() const
{
    std::vector<InterruptedOperation> interrupted;
    (void)interrupted_operations(interrupted);
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
        {},
    };
    return finish(operation.operation_id, operation.kind, result);
}

} // namespace infiltrator::files
