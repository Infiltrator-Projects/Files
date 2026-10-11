// SPDX-License-Identifier: GPL-3.0-or-later
#include "destination_ownership.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

bool require(const bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

std::string read_text(const std::filesystem::path &path)
{
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

bool write_text(const std::filesystem::path &path, const std::string &text)
{
    std::ofstream output(path);
    output << text;
    return output.good();
}

bool unsupported_eopnotsupp(const std::filesystem::path &,
                            const std::filesystem::path &,
                            std::error_code &error) noexcept
{
    error = std::error_code(EOPNOTSUPP, std::generic_category());
    return false;
}

bool unsupported_enotsup(const std::filesystem::path &,
                         const std::filesystem::path &,
                         std::error_code &error) noexcept
{
    error = std::error_code(ENOTSUP, std::generic_category());
    return false;
}

infiltrator::files::destination_ownership::Identity failing_identity_for_fd(
    int,
    std::error_code &error) noexcept
{
    error = std::make_error_code(std::errc::io_error);
    return {};
}

std::uintmax_t denied_remove_all(const std::filesystem::path &,
                                 std::error_code &error)
{
    error = std::make_error_code(std::errc::permission_denied);
    return 0;
}

bool has_internal_residue(const std::filesystem::path &root)
{
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(root, error), end;
         !error && iterator != end;
         iterator.increment(error)) {
        if (iterator->path().filename().string().starts_with(".infiltrator-")) {
            return true;
        }
    }
    return static_cast<bool>(error);
}

} // namespace

int main()
{
    namespace ownership = infiltrator::files::destination_ownership;
    namespace fs = std::filesystem;

    const auto root = fs::temp_directory_path() /
                      ("infiltrator-destination-ownership-" +
                       std::to_string(static_cast<long long>(::getpid())));
    std::error_code error;
    fs::remove_all(root, error);
    error.clear();
    if (!require(fs::create_directories(root, error) && !error,
                 "could not create test root")) {
        return 1;
    }

    const auto source = root / "source";
    const auto destination = root / "destination";
    if (!require(write_text(source, "source") &&
                     write_text(destination, "competing writer"),
                 "could not create rename fixture")) {
        return 2;
    }

    const bool renamed = ownership::rename_no_replace(source, destination, error);
    if (!require(!renamed && error == std::errc::file_exists &&
                     read_text(source) == "source" &&
                     read_text(destination) == "competing writer",
                 "no-replace rename overwrote a competing destination")) {
        return 3;
    }

    error.clear();
    const int contested = ownership::open_exclusive(destination, error);
    if (!require(contested < 0 && error == std::errc::file_exists &&
                     read_text(destination) == "competing writer",
                 "exclusive file creation accepted a competing destination")) {
        if (contested >= 0) {
            (void)::close(contested);
        }
        return 4;
    }

    const auto competing_directory = root / "competing-directory";
    error.clear();
    if (!require(fs::create_directory(competing_directory, error) && !error,
                 "could not create competing directory fixture")) {
        return 5;
    }
    error.clear();
    if (!require(!ownership::create_directory_exclusive(competing_directory, error) &&
                     error == std::errc::file_exists,
                 "exclusive directory creation accepted a competing directory")) {
        return 6;
    }

    const auto owned_directory = root / "owned-directory";
    error.clear();
    if (!require(ownership::create_directory_exclusive(owned_directory, error),
                 "could not create owned directory")) {
        return 7;
    }
    ownership::OwnedOutputs directory_outputs;
    if (!require(ownership::record(owned_directory, directory_outputs, error),
                 "could not record owned directory")) {
        return 8;
    }

    const auto owned_file = owned_directory / "owned";
    const int descriptor = ownership::open_exclusive(owned_file, error);
    if (!require(descriptor >= 0, "could not create owned file")) {
        return 9;
    }
    if (!require(ownership::record_fd(owned_file, descriptor, directory_outputs, error) &&
                     ::write(descriptor, "owned", 5) == 5 &&
                     ::close(descriptor) == 0,
                 "could not record or write owned file")) {
        return 10;
    }

    const auto competing_file = owned_directory / "competing";
    if (!require(write_text(competing_file, "competing writer"),
                 "could not create nested competitor")) {
        return 11;
    }
    error.clear();
    std::vector<fs::path> retained_cleanup_paths;
    if (!require(!ownership::cleanup_owned(
                         directory_outputs, error, &retained_cleanup_paths) &&
                     retained_cleanup_paths.size() == 1U &&
                     retained_cleanup_paths.front() == owned_directory &&
                     !fs::exists(owned_file) &&
                     read_text(competing_file) == "competing writer" &&
                     fs::is_directory(owned_directory),
                 "cleanup removed or displaced non-owned directory content")) {
        return 12;
    }

    const auto raced_path = root / "raced";
    const auto displaced_owned = root / "displaced-owned";
    if (!require(write_text(raced_path, "owned"), "could not create race fixture")) {
        return 13;
    }
    ownership::OwnedOutputs raced_outputs;
    if (!require(ownership::record(raced_path, raced_outputs, error),
                 "could not record race fixture identity")) {
        return 14;
    }
    error.clear();
    fs::rename(raced_path, displaced_owned, error);
    if (!require(!error && write_text(raced_path, "competing writer"),
                 "could not install competing replacement")) {
        return 15;
    }
    error.clear();
    if (!require(!ownership::cleanup_owned(raced_outputs, error) &&
                     read_text(raced_path) == "competing writer" &&
                     read_text(displaced_owned) == "owned",
                 "cleanup deleted the object that replaced an owned pathname")) {
        return 16;
    }

    const auto clean_path = root / "clean-owned";
    if (!require(write_text(clean_path, "owned"), "could not create cleanup fixture")) {
        return 17;
    }
    ownership::OwnedOutputs clean_outputs;
    if (!require(ownership::record(clean_path, clean_outputs, error),
                 "could not record cleanup fixture")) {
        return 18;
    }
    error.clear();
    if (!require(ownership::cleanup_owned(clean_outputs, error) &&
                     !fs::exists(clean_path),
                 "cleanup did not remove an unchanged owned object")) {
        return 19;
    }

    const auto staged = root / "staged-previous-destination";
    if (!require(write_text(staged, "previous destination"),
                 "could not create rollback fixture")) {
        return 20;
    }
    error.clear();
    if (!require(!ownership::rename_no_replace(staged, destination, error) &&
                     error == std::errc::file_exists &&
                     read_text(staged) == "previous destination" &&
                     read_text(destination) == "competing writer",
                 "rollback displaced a competing destination")) {
        return 21;
    }

    const auto proof_root = root / "proof-root";
    const auto proof_child = proof_root / "child";
    const auto displaced_child = root / "displaced-proof-child";
    error.clear();
    if (!require(fs::create_directory(proof_root, error) &&
                     write_text(proof_child, "owned child"),
                 "could not create complete-proof fixture")) {
        return 22;
    }
    ownership::OwnedOutputs proof_outputs;
    if (!require(ownership::record(proof_root, proof_outputs, error) &&
                     ownership::record(proof_child, proof_outputs, error),
                 "could not record complete-proof fixture")) {
        return 23;
    }
    fs::rename(proof_child, displaced_child, error);
    if (!require(!error && write_text(proof_child, "competing child"),
                 "could not replace a copied child")) {
        return 24;
    }
    error.clear();
    if (!require(!ownership::verify_all(proof_outputs, error) &&
                     read_text(proof_child) == "competing child",
                 "complete ownership proof accepted a replaced child")) {
        return 25;
    }

    const auto backup = root / "owned-backup";
    const auto displaced_backup = root / "displaced-backup";
    const auto restore_destination = root / "restore-destination";
    if (!require(write_text(backup, "staged original"),
                 "could not create staged-backup fixture")) {
        return 26;
    }
    ownership::OwnedOutputs staged_outputs;
    if (!require(ownership::record(backup, staged_outputs, error),
                 "could not record staged-backup identity")) {
        return 27;
    }
    fs::rename(backup, displaced_backup, error);
    if (!require(!error && write_text(backup, "competing backup"),
                 "could not replace staged-backup pathname")) {
        return 28;
    }
    error.clear();
    if (!require(!ownership::restore_owned(staged_outputs.front(), restore_destination, error) &&
                     !fs::exists(restore_destination) &&
                     read_text(backup) == "competing backup" &&
                     read_text(displaced_backup) == "staged original",
                 "rollback restored or displaced a non-owned staged backup")) {
        return 29;
    }

    const auto cleanup_backup = root / "cleanup-backup";
    const auto displaced_cleanup_backup = root / "displaced-cleanup-backup";
    if (!require(write_text(cleanup_backup, "owned backup"),
                 "could not create backup-cleanup fixture")) {
        return 30;
    }
    ownership::OwnedOutputs cleanup_backup_outputs;
    if (!require(ownership::record(cleanup_backup, cleanup_backup_outputs, error),
                 "could not record backup-cleanup fixture")) {
        return 31;
    }
    fs::rename(cleanup_backup, displaced_cleanup_backup, error);
    if (!require(!error && write_text(cleanup_backup, "competing backup"),
                 "could not replace backup before cleanup")) {
        return 32;
    }
    error.clear();
    if (!require(!ownership::remove_owned_tree(cleanup_backup_outputs.front(), error) &&
                     read_text(cleanup_backup) == "competing backup" &&
                     read_text(displaced_cleanup_backup) == "owned backup",
                 "successful replacement cleanup deleted a competing backup")) {
        return 33;
    }

    const auto private_public = root / "published-tree";
    ownership::OwnedOutputs private_outputs;
    ownership::ProtectedStaging private_staging;
    const auto private_root =
        ownership::create_private_directory(
            private_public, private_staging, private_outputs, error);
    if (!require(!private_root.empty() && write_text(private_root / "child", "private"),
                 "could not create private publication fixture")) {
        return 34;
    }
    if (!require(write_text(private_public, "competing writer"),
                 "could not claim publication destination")) {
        return 35;
    }
    error.clear();
    if (!require(!ownership::publish_private_tree(
                     private_staging, private_root, private_public, private_outputs, error) &&
                     error == std::errc::file_exists &&
                     read_text(private_public) == "competing writer" &&
                     read_text(private_root / "child") == "private",
                 "private tree publication displaced a competing writer")) {
        return 36;
    }

    const auto move_source = root / "move-source";
    const auto displaced_move_source = root / "displaced-move-source";
    const auto move_target = root / "move-target";
    if (!require(write_text(move_source, "original"), "could not create move fixture")) {
        return 37;
    }
    ownership::OwnedOutputs move_outputs;
    if (!require(ownership::record(move_source, move_outputs, error),
                 "could not record move source")) {
        return 38;
    }
    fs::rename(move_source, displaced_move_source, error);
    if (!require(!error && write_text(move_source, "replacement"),
                 "could not swap move source")) {
        return 39;
    }
    ownership::OwnedOutput moved;
    error.clear();
    if (!require(!ownership::move_owned_no_replace(
                     move_outputs.front(), move_target, moved, error) &&
                     !fs::exists(move_target) &&
                     read_text(move_source) == "replacement" &&
                     read_text(displaced_move_source) == "original",
                 "identity-safe move displaced a swapped source")) {
        return 40;
    }

    const auto remaining_quarantine = root / ".infiltrator-cleanup-partial";
    const auto restored_source = root / "restored-source";
    if (!require(fs::create_directory(remaining_quarantine, error) &&
                     write_text(remaining_quarantine / "remaining", "data"),
                 "could not create partial-removal fixture")) {
        return 41;
    }
    error.clear();
    if (!require(ownership::restore_quarantine(
                     remaining_quarantine, restored_source, error) &&
                     read_text(restored_source / "remaining") == "data" &&
                     !fs::exists(remaining_quarantine),
                 "remaining quarantined source data was not restored")) {
        return 42;
    }

    const auto retained_quarantine = root / ".infiltrator-cleanup-retained";
    if (!require(write_text(retained_quarantine, "remaining") &&
                     write_text(move_target, "new source occupant"),
                 "could not create unsafe-restoration fixture")) {
        return 43;
    }
    error.clear();
    if (!require(!ownership::restore_quarantine(
                     retained_quarantine, move_target, error) &&
                     error == std::errc::file_exists &&
                     read_text(retained_quarantine) == "remaining" &&
                     read_text(move_target) == "new source occupant",
                 "unsafe source restoration displaced another writer")) {
        return 44;
    }

    const auto symlink_public = root / "published-link";
    ownership::OwnedOutputs symlink_outputs;
    ownership::ProtectedStaging symlink_staging;
    error.clear();
    const auto private_link = ownership::create_private_symlink(
        symlink_public, fs::path{"owned-target"}, symlink_staging, symlink_outputs, error);
    if (!require(!private_link.empty() && !error,
                 "could not create private symlink fixture")) {
        return 45;
    }
    fs::create_symlink("competing-target", symlink_public, error);
    if (!require(!error, "could not claim symlink publication destination")) {
        return 46;
    }
    error.clear();
    if (!require(!ownership::publish_private_output(
                     symlink_staging, private_link, symlink_public, symlink_outputs.front(), error) &&
                     error == std::errc::file_exists &&
                     fs::read_symlink(symlink_public, error) == "competing-target" &&
                     !error,
                 "private symlink publication displaced a competing writer")) {
        return 47;
    }
    error.clear();
    const bool symlink_cleaned = ownership::cleanup_owned(symlink_outputs, error);
    std::error_code link_state_error;
    const auto link_state = fs::symlink_status(private_link, link_state_error);
    const bool private_link_gone =
        link_state.type() == fs::file_type::not_found ||
        link_state_error == std::errc::no_such_file_or_directory;
    if (!require(symlink_cleaned && private_link_gone,
                 "private symlink cleanup did not remove the owned temporary link")) {
        return 48;
    }

    const auto private_file_public = root / "private-file-public";
    fs::path private_file_stage;
    {
        ownership::ProtectedStaging private_file_staging;
        ownership::OwnedOutputs private_file_outputs;
        std::filesystem::path private_file_path;
        error.clear();
        const int private_file_descriptor = ownership::create_private_file(
            private_file_public,
            private_file_staging,
            private_file_outputs,
            private_file_path,
            error,
            failing_identity_for_fd);
        private_file_stage = private_file_staging.container_path;
        if (!require(private_file_descriptor < 0 &&
                         error == std::errc::io_error &&
                         private_file_outputs.empty() &&
                         private_file_path.empty() &&
                         !fs::exists(private_file_public),
                     "record failure left a public or owned regular-file residue")) {
            return 81;
        }
    }
    if (!require(!fs::exists(private_file_stage),
                 "record failure retained private regular-file staging residue")) {
        return 82;
    }

    const auto partial_cleanup = root / "partial-cleanup";
    const auto partial_child = partial_cleanup / "child";
    error.clear();
    if (!require(fs::create_directory(partial_cleanup, error) &&
                     write_text(partial_child, "retained"),
                 "could not create partial tree cleanup fixture")) {
        return 49;
    }
    ownership::OwnedOutputs partial_outputs;
    if (!require(ownership::record(partial_cleanup, partial_outputs, error),
                 "could not record partial tree cleanup fixture")) {
        return 50;
    }
    std::filesystem::path retained_cleanup;
    error.clear();
    const bool removed_partial = ownership::remove_owned_tree(
        partial_outputs.front(), error, &retained_cleanup, denied_remove_all);
    if (!require(!removed_partial &&
                     error == std::errc::permission_denied &&
                     retained_cleanup.empty() &&
                     fs::is_directory(partial_cleanup) &&
                     fs::exists(partial_child),
                 "injected tree cleanup failure did not restore remaining owned data")) {
        return 52;
    }

    const auto unsupported_root_one = root / "unsupported-eopnotsupp";
    error.clear();
    if (!require(fs::create_directory(unsupported_root_one, error) && !error,
                 "could not create EOPNOTSUPP root")) {
        return 60;
    }
    const auto unsupported_source_one = unsupported_root_one / "source";
    const auto unsupported_target_one = unsupported_root_one / "target";
    if (!require(write_text(unsupported_source_one, "untouched"),
                 "could not create EOPNOTSUPP fixture")) {
        return 61;
    }
    ownership::OwnedOutputs unsupported_outputs_one;
    if (!require(ownership::record(unsupported_source_one, unsupported_outputs_one, error),
                 "could not record EOPNOTSUPP fixture")) {
        return 62;
    }
    ownership::OwnedOutput unsupported_moved;
    error.clear();
    if (!require(!ownership::move_owned_no_replace(unsupported_outputs_one.front(),
                                                    unsupported_target_one,
                                                    unsupported_moved,
                                                    error,
                                                    nullptr,
                                                    unsupported_eopnotsupp) &&
                     error == std::errc::operation_not_supported &&
                     read_text(unsupported_source_one) == "untouched" &&
                     !fs::exists(unsupported_target_one) &&
                     !has_internal_residue(unsupported_root_one),
                 "EOPNOTSUPP capability failure mutated the source, destination, or hidden state")) {
        return 63;
    }

    const auto unsupported_root_two = root / "unsupported-enotsup";
    error.clear();
    if (!require(fs::create_directory(unsupported_root_two, error) && !error,
                 "could not create ENOTSUP root")) {
        return 64;
    }
    const auto unsupported_source_two = unsupported_root_two / "source";
    const auto unsupported_target_two = unsupported_root_two / "target";
    if (!require(write_text(unsupported_source_two, "untouched"),
                 "could not create ENOTSUP fixture")) {
        return 65;
    }
    ownership::OwnedOutputs unsupported_outputs_two;
    if (!require(ownership::record(unsupported_source_two, unsupported_outputs_two, error),
                 "could not record ENOTSUP fixture")) {
        return 66;
    }
    error.clear();
    if (!require(!ownership::move_owned_no_replace(unsupported_outputs_two.front(),
                                                    unsupported_target_two,
                                                    unsupported_moved,
                                                    error,
                                                    nullptr,
                                                    unsupported_enotsup) &&
                     error == std::errc::operation_not_supported &&
                     read_text(unsupported_source_two) == "untouched" &&
                     !fs::exists(unsupported_target_two) &&
                     !has_internal_residue(unsupported_root_two),
                 "ENOTSUP capability failure mutated the source, destination, or hidden state")) {
        return 67;
    }



    // A replacement of the visible staging-container pathname must not redirect
    // writes or publication away from the directory descriptor Files owns.
    const auto protected_public = root / "protected-published-tree";
    const auto displaced_stage = root / "displaced-stage-container";
    ownership::OwnedOutputs protected_outputs;
    ownership::ProtectedStaging protected_staging;
    error.clear();
    const auto protected_root = ownership::create_private_directory(
        protected_public, protected_staging, protected_outputs, error);
    if (!require(!protected_root.empty() && !error,
                 "could not create protected staging fixture")) {
        return 68;
    }
    const auto visible_stage = protected_staging.container_path;
    fs::rename(visible_stage, displaced_stage, error);
    if (!require(!error && fs::create_directory(visible_stage, error) &&
                     write_text(visible_stage / "competitor", "competitor"),
                 "could not replace visible staging container")) {
        return 69;
    }
    if (!require(write_text(protected_root / "child", "owned") &&
                     ownership::record(protected_root / "child", protected_outputs, error),
                 "protected staging path no longer referenced the owned directory")) {
        return 70;
    }
    error.clear();
    if (!require(ownership::publish_private_tree(protected_staging,
                                                  protected_root,
                                                  protected_public,
                                                  protected_outputs,
                                                  error) &&
                     read_text(protected_public / "child") == "owned" &&
                     read_text(visible_stage / "competitor") == "competitor",
                 "visible staging replacement redirected private publication")) {
        return 71;
    }

    // Once owned data is quarantined, replacing the visible staging-container
    // name must not redirect deletion; the open staging descriptor remains the
    // authority for the quarantined payload.
    const auto protected_cleanup_source = root / "protected-cleanup-source";
    const auto displaced_cleanup_stage = root / "displaced-cleanup-stage";
    if (!require(write_text(protected_cleanup_source, "owned cleanup"),
                 "could not create protected cleanup fixture")) {
        return 72;
    }
    ownership::OwnedOutputs protected_cleanup_outputs;
    if (!require(ownership::record(protected_cleanup_source,
                                   protected_cleanup_outputs,
                                   error),
                 "could not record protected cleanup identity")) {
        return 73;
    }
    ownership::ProtectedStaging cleanup_staging;
    std::filesystem::path cleanup_quarantine;
    std::filesystem::path cleanup_retained;
    error.clear();
    if (!require(ownership::quarantine_owned(protected_cleanup_outputs.front(),
                                             cleanup_staging,
                                             cleanup_quarantine,
                                             error,
                                             &cleanup_retained),
                 "could not quarantine protected cleanup fixture")) {
        return 74;
    }
    const auto visible_cleanup_stage = cleanup_staging.container_path;
    fs::rename(visible_cleanup_stage, displaced_cleanup_stage, error);
    if (!require(!error && fs::create_directory(visible_cleanup_stage, error) &&
                     write_text(visible_cleanup_stage / "competitor", "competitor"),
                 "could not replace cleanup staging container")) {
        return 75;
    }
    error.clear();
    if (!require(fs::remove(cleanup_quarantine, error) && !error &&
                     read_text(visible_cleanup_stage / "competitor") == "competitor",
                 "quarantine cleanup removed a competing staging replacement")) {
        return 76;
    }
    ownership::release_staging(cleanup_staging);

    // If the original source name is claimed before a protected quarantine can
    // be restored, the displaced data is published to an exact retained path.
    const auto retained_source = root / "retained-source";
    if (!require(write_text(retained_source, "owned retained"),
                 "could not create retained-source fixture")) {
        return 77;
    }
    ownership::OwnedOutputs retained_outputs;
    if (!require(ownership::record(retained_source, retained_outputs, error),
                 "could not record retained-source identity")) {
        return 78;
    }
    ownership::ProtectedStaging retained_staging;
    std::filesystem::path retained_payload;
    std::filesystem::path retained_path;
    error.clear();
    if (!require(ownership::quarantine_owned(retained_outputs.front(),
                                             retained_staging,
                                             retained_payload,
                                             error,
                                             &retained_path) &&
                     write_text(retained_source, "competitor"),
                 "could not prepare retained-source restoration race")) {
        return 79;
    }
    error.clear();
    if (!require(!ownership::restore_quarantine(retained_staging,
                                                retained_source,
                                                error,
                                                &retained_path) &&
                     error == std::errc::file_exists &&
                     !retained_path.empty() &&
                     read_text(retained_source) == "competitor" &&
                     read_text(retained_path) == "owned retained",
                 "blocked source restoration did not surface retained data")) {
        return 80;
    }

    const auto previous_directory = fs::current_path(error);
    if (!require(!error, "could not capture current directory")) {
        return 54;
    }
    fs::current_path(root, error);
    if (!require(!error && write_text("relative-source", "relative payload"),
                 "could not create relative rename fixture")) {
        return 55;
    }
    ownership::OwnedOutputs relative_outputs;
    if (!require(ownership::record("relative-source", relative_outputs, error),
                 "could not record relative source identity")) {
        return 56;
    }
    ownership::OwnedOutput relative_moved;
    error.clear();
    if (!require(ownership::move_owned_no_replace(relative_outputs.front(),
                                                   "relative-destination",
                                                   relative_moved,
                                                   error) &&
                     read_text("relative-destination") == "relative payload" &&
                     !fs::exists("relative-source"),
                 "relative ownership move failed")) {
        return 57;
    }
    fs::current_path(previous_directory, error);
    if (!require(!error, "could not restore current directory")) {
        return 58;
    }

    error.clear();
    fs::remove_all(root, error);
    return require(!error, "could not remove test root") ? 0 : 59;
}
