// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_journal.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

using infiltrator::files::InterruptedOperation;
using infiltrator::files::OperationJournal;
using infiltrator::files::OperationPhase;
using infiltrator::files::OperationResult;
using infiltrator::files::OperationStatus;

namespace {

std::optional<std::string> environment_value(const char *name)
{
    const char *value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
}

void restore_environment(const char *name, const std::optional<std::string> &value)
{
    if (value.has_value()) {
        (void)::setenv(name, value->c_str(), 1);
    } else {
        (void)::unsetenv(name);
    }
}

} // namespace

int main()
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
                          ("infiltrator-files-journal-test-" + std::to_string(::getpid()));
    const fs::path journal_path = root / "state" / "operations.log";
    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);

    OperationJournal journal(journal_path);
    const std::string id = journal.begin("copy", "/tmp/source\tname", "/tmp/destination");
    if (id.empty() || !fs::exists(journal_path)) {
        fs::remove_all(root, cleanup_error);
        return 1;
    }

    // An operation begun by this live application session must never be
    // reclassified as crash residue merely because another window scans the
    // shared journal before the operation has finished.
    std::vector<InterruptedOperation> interrupted;
    if (!journal.interrupted_operations(interrupted) || !interrupted.empty()) {
        fs::remove_all(root, cleanup_error);
        return 2;
    }

    const OperationResult result{
        OperationStatus::Success,
        OperationPhase::Complete,
        "/tmp/destination/source",
        "Copied successfully.",
        true,
    };
    if (!journal.finish(id, "copy", result)) {
        fs::remove_all(root, cleanup_error);
        return 3;
    }

    std::ifstream stream(journal_path, std::ios::binary);
    std::ostringstream content;
    content << stream.rdbuf();
    std::string text = content.str();
    if (text.find("\t" + id + "\tSTART\tcopy\t/tmp/source\\tname\t/tmp/destination") == std::string::npos ||
        text.find("\t" + id + "\tEND\tcopy\tsuccess\tchanged\t/tmp/destination/source\tCopied successfully.") == std::string::npos) {
        fs::remove_all(root, cleanup_error);
        return 4;
    }

    // Model a complete START record from an earlier application session,
    // followed by an incomplete crash tail. The scan must expose only the
    // complete earlier record, and the next journal append must discard the
    // damaged tail before writing a new framed record.
    {
        std::ofstream append(journal_path, std::ios::binary | std::ios::app);
        append << "100\tprevious-session-1\tSTART\tmove\t/tmp/interrupted\\nsource\t/tmp/interrupted-destination\n";
        append << "101\tdamaged-tail\tSTART\tcopy\t/tmp/partial";
    }

    if (!journal.interrupted_operations(interrupted) ||
        interrupted.size() != 1U ||
        interrupted.front().operation_id != "previous-session-1" ||
        interrupted.front().kind != "move" ||
        interrupted.front().source != "/tmp/interrupted\nsource" ||
        interrupted.front().destination != "/tmp/interrupted-destination") {
        fs::remove_all(root, cleanup_error);
        return 5;
    }

    if (!journal.mark_interrupted(interrupted.front()) ||
        !journal.interrupted_operations(interrupted) || !interrupted.empty()) {
        fs::remove_all(root, cleanup_error);
        return 6;
    }

    std::ifstream recovered_stream(journal_path, std::ios::binary);
    std::ostringstream recovered_content;
    recovered_content << recovered_stream.rdbuf();
    text = recovered_content.str();
    if (text.find("damaged-tail") != std::string::npos ||
        text.find("\tprevious-session-1\tEND\tmove\tverification-failure\tchanged") == std::string::npos ||
        text.find("previous Files session ended before this operation recorded completion") == std::string::npos) {
        fs::remove_all(root, cleanup_error);
        return 7;
    }

    // Another live Files process must not be misclassified as a crash.
    std::ostringstream live_pid;
    live_pid << std::hex << static_cast<unsigned long>(::getpid());
    const std::string other_live_id = "ab123-" + live_pid.str() + "-cd456-7";
    {
        std::ofstream append(journal_path, std::ios::binary | std::ios::app);
        append << "102\t" << other_live_id
               << "\tSTART\tcopy\t/tmp/live-source\t/tmp/live-destination\n";
    }
    if (!journal.interrupted_operations(interrupted) || !interrupted.empty()) {
        fs::remove_all(root, cleanup_error);
        return 12;
    }
    const OperationResult live_result{
        OperationStatus::Success, OperationPhase::Complete,
        "/tmp/live-destination", "Completed.", true};
    if (!journal.finish(other_live_id, "copy", live_result)) {
        fs::remove_all(root, cleanup_error);
        return 13;
    }

    // A complete but malformed record is unhealthy, not silently ignored.
    const fs::path malformed_path = root / "state" / "malformed.log";
    {
        std::ofstream corrupt(malformed_path, std::ios::binary);
        corrupt << "103\tmalformed\tSTART\tcopy\tmissing-destination\n";
    }
    OperationJournal malformed(malformed_path);
    if (malformed.interrupted_operations(interrupted)) {
        fs::remove_all(root, cleanup_error);
        return 14;
    }

    const auto permissions = fs::status(journal_path).permissions();
    const auto group_or_other = fs::perms::group_read | fs::perms::group_write |
                                fs::perms::group_exec | fs::perms::others_read |
                                fs::perms::others_write | fs::perms::others_exec;
    if ((permissions & group_or_other) != fs::perms::none) {
        fs::remove_all(root, cleanup_error);
        return 8;
    }

    // A durable journal must never silently fall back to /tmp when no absolute
    // persistent state/home location can be established.
    const auto old_state_home = environment_value("XDG_STATE_HOME");
    const auto old_home = environment_value("HOME");
    (void)::unsetenv("XDG_STATE_HOME");
    (void)::unsetenv("HOME");
    const fs::path missing_default = OperationJournal::default_path();
    (void)::setenv("XDG_STATE_HOME", "relative-state", 1);
    const fs::path relative_default = OperationJournal::default_path();
    restore_environment("XDG_STATE_HOME", old_state_home);
    restore_environment("HOME", old_home);
    if (!missing_default.empty() || !relative_default.empty()) {
        fs::remove_all(root, cleanup_error);
        return 9;
    }

    OperationJournal unavailable(fs::path{});
    if (!unavailable.begin("rename", "/tmp/a", "/tmp/b").empty()) {
        fs::remove_all(root, cleanup_error);
        return 10;
    }
    interrupted.clear();
    if (unavailable.interrupted_operations(interrupted)) {
        fs::remove_all(root, cleanup_error);
        return 11;
    }

    fs::remove_all(root, cleanup_error);
    return 0;
}
