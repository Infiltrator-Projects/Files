// SPDX-License-Identifier: GPL-3.0-or-later
#include "batch_removal_engine.hpp"
#include "operation_journal.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

using namespace infiltrator::files;
namespace fs = std::filesystem;
namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void write(const fs::path &path) { std::ofstream(path) << "payload"; }
}
int main()
{
    const auto root = fs::temp_directory_path() / ("files-batch-removal-" + std::to_string(::getpid()));
    fs::create_directories(root / "work"); fs::create_directories(root / "state");
    fs::create_directories(root / "data");
    ::setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);
    ::setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
    try {
        const auto a = root / "work" / "a", b = root / "work" / "b";
        write(a); write(b);
        BatchRemovalEngine engine;
        auto operation = engine.execute({a, root / "work" / "missing"}, BatchRemovalKind::PermanentDelete);
        require(!operation.ok() && !operation.changed && fs::exists(a) && fs::exists(b),
                "Every source must be preflighted before a batch deletes any item");
        operation = engine.execute({a, a}, BatchRemovalKind::PermanentDelete);
        require(!operation.ok() && !operation.changed && fs::exists(a), "Duplicate sources must be rejected before mutation");
        operation = engine.execute({a, b}, BatchRemovalKind::PermanentDelete, [] { return true; });
        require(operation.status == OperationStatus::Cancelled && !operation.changed && fs::exists(a) && fs::exists(b),
                "Early cancellation must not remove any selected item");
        unsigned completed = 0U;
        operation = engine.execute({a, b}, BatchRemovalKind::PermanentDelete,
            [&] { return completed != 0U; }, [&] { ++completed; });
        require(operation.status == OperationStatus::Cancelled && operation.changed && completed == 1U &&
                !fs::exists(a) && fs::exists(b) && operation.message.find("1 of 2") != std::string::npos,
                "Cancellation between items must report the completed count and leave later items intact");
        fs::create_directories(a / "nested"); write(a / "nested" / "file");
        const auto outside = root / "outside"; write(outside);
        fs::create_symlink(outside, a / "external-link");
        operation = engine.execute({a, b}, BatchRemovalKind::PermanentDelete);
        require(operation.ok() && operation.changed && !fs::exists(a) && !fs::exists(b) && fs::exists(outside),
                "Recursive batch deletion must remove selected trees without following their symlinks");
        write(a); write(b);
        operation = engine.execute({a, b}, BatchRemovalKind::Trash);
        require(operation.ok() && !fs::exists(a) && !fs::exists(b) &&
                fs::exists(root / "data" / "Trash" / "files" / "a") &&
                fs::exists(root / "data" / "Trash" / "files" / "b"),
                "Batch Trash must use recoverable platform Trash storage");
        OperationJournal journal;
        require(journal.interrupted_operations().empty(), "Every completed batch item must have a durable END record");
        write(a); write(b);
        std::ofstream(root / "state" / "files" / "operations.log", std::ios::app) << "malformed complete frame\n";
        operation = engine.execute({a, b}, BatchRemovalKind::PermanentDelete);
        require(operation.status == OperationStatus::VerificationFailure && !operation.changed && fs::exists(a) && fs::exists(b),
                "Malformed journal state must refuse destructive batch admission");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
    fs::remove_all(root); return 0;
}
