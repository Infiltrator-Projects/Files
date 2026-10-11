from pathlib import Path
import re

header = Path('src/core/destination_ownership.hpp')
text = header.read_text()


def replace_once(source, old, new, label):
    count = source.count(old)
    if count != 1:
        raise SystemExit(f'{label}: expected one exact match, found {count}')
    return source.replace(old, new, 1)


def sub_once(source, pattern, replacement, label):
    updated, count = re.subn(pattern, replacement, source, count=1, flags=re.S)
    if count != 1:
        raise SystemExit(f'{label}: expected one regex match, found {count}')
    return updated


text = replace_once(text, '#include <unistd.h>\n', '#include <unistd.h>\n#include <stdlib.h>\n', 'stdlib include')

old_alias = '''using RenameNoReplaceOperation = bool (*)(const std::filesystem::path &,
                                          const std::filesystem::path &,
                                          std::error_code &) noexcept;
'''
new_alias = old_alias + '''
struct ProtectedStaging;
inline void release_staging(ProtectedStaging &staging) noexcept;

struct ProtectedStaging {
    std::filesystem::path container_path;
    int descriptor{-1};
    Identity identity{};

    ProtectedStaging() = default;
    ProtectedStaging(const ProtectedStaging &) = delete;
    ProtectedStaging &operator=(const ProtectedStaging &) = delete;
    ~ProtectedStaging() { release_staging(*this); }

    [[nodiscard]] std::filesystem::path access_root() const
    {
        return descriptor >= 0
                   ? std::filesystem::path{"/proc/self/fd"} / std::to_string(descriptor)
                   : std::filesystem::path{};
    }
};
'''
text = replace_once(text, old_alias, new_alias, 'ProtectedStaging declaration')

record_fd_pattern = r'''inline bool record_fd\(const std::filesystem::path &path,\n                      const int descriptor,\n                      OwnedOutputs &outputs,\n                      std::error_code &error\) noexcept\n\{.*?\n\}\n'''
match = re.search(record_fd_pattern, text, flags=re.S)
if not match:
    raise SystemExit('record_fd block not found')
helper_block = match.group(0) + r'''

inline Identity identity_for_at(const int directory_descriptor,
                                const char *name,
                                std::error_code &error) noexcept
{
    struct stat status {};
    if (::fstatat(directory_descriptor, name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
        error = std::error_code(errno, std::generic_category());
        return {};
    }
    error.clear();
    return Identity{static_cast<std::uintmax_t>(status.st_dev),
                    static_cast<std::uintmax_t>(status.st_ino),
                    true};
}

inline bool record_at(const int directory_descriptor,
                      const char *name,
                      const std::filesystem::path &display_path,
                      OwnedOutputs &outputs,
                      std::error_code &error) noexcept
{
    const Identity identity = identity_for_at(directory_descriptor, name, error);
    if (error) {
        return false;
    }
    outputs.push_back(OwnedOutput{display_path, identity});
    return true;
}

inline void release_staging(ProtectedStaging &staging) noexcept
{
    const int descriptor = staging.descriptor;
    const auto container_path = staging.container_path;
    const Identity expected = staging.identity;

    staging.descriptor = -1;
    staging.container_path.clear();
    staging.identity = {};

    if (descriptor < 0) {
        return;
    }

    std::error_code inspect_error;
    if (!container_path.empty() && expected.valid &&
        same_object(container_path, expected, inspect_error)) {
        std::error_code remove_error;
        (void)std::filesystem::remove(container_path, remove_error);
    }
    (void)::close(descriptor);
}

inline bool create_protected_staging(const std::filesystem::path &parent_path,
                                     ProtectedStaging &staging,
                                     std::error_code &error) noexcept
{
    release_staging(staging);
    const std::filesystem::path parent = parent_path.empty()
                                             ? std::filesystem::path{"."}
                                             : parent_path;
    const std::string pattern =
        (parent / (".infiltrator-stage-" +
                   std::to_string(static_cast<long long>(::getpid())) + "-XXXXXX"))
            .string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    char *created = ::mkdtemp(buffer.data());
    if (created == nullptr) {
        error = std::error_code(errno, std::generic_category());
        return false;
    }

    staging.container_path = std::filesystem::path{created};
    staging.descriptor =
        ::open(staging.container_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (staging.descriptor < 0) {
        error = std::error_code(errno, std::generic_category());
        staging.container_path.clear();
        return false;
    }

    staging.identity = identity_for_fd(staging.descriptor, error);
    if (error || !staging.identity.valid) {
        release_staging(staging);
        return false;
    }

    std::error_code verify_error;
    if (!same_object(staging.container_path, staging.identity, verify_error)) {
        const std::error_code result = verify_error
                                           ? verify_error
                                           : std::make_error_code(std::errc::state_not_recoverable);
        (void)::close(staging.descriptor);
        staging.descriptor = -1;
        staging.container_path.clear();
        staging.identity = {};
        error = result;
        return false;
    }

    error.clear();
    return true;
}
'''
text = text[:match.start()] + helper_block + text[match.end():]

rename_pattern = r'''inline bool rename_no_replace\(const std::filesystem::path &source,\n                              const std::filesystem::path &destination,\n                              std::error_code &error\) noexcept\n\{.*?\n\}\n'''
rename_replacement = r'''inline bool rename_no_replace_at(const int source_directory,
                                 const char *source_name,
                                 const int destination_directory,
                                 const char *destination_name,
                                 std::error_code &error) noexcept
{
#if defined(__linux__) && defined(SYS_renameat2)
    if (::syscall(SYS_renameat2,
                  source_directory,
                  source_name,
                  destination_directory,
                  destination_name,
                  RENAME_NOREPLACE) == 0) {
        error.clear();
        return true;
    }

    const int rename_error = errno;
    if (rename_error != ENOSYS && rename_error != EINVAL &&
        rename_error != EOPNOTSUPP && rename_error != ENOTSUP) {
        error = std::error_code(rename_error, std::generic_category());
        return false;
    }
#endif
    error = std::make_error_code(std::errc::operation_not_supported);
    return false;
}

inline bool rename_no_replace(const std::filesystem::path &source,
                              const std::filesystem::path &destination,
                              std::error_code &error) noexcept
{
    return rename_no_replace_at(
        AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(), error);
}
'''
text = sub_once(text, rename_pattern, rename_replacement, 'rename_no_replace')

marker = '''inline bool create_directory_exclusive(const std::filesystem::path &path,
                                       std::error_code &error) noexcept
'''
retained_helper = r'''inline bool retain_staged_payload(ProtectedStaging &staging,
                                  const std::filesystem::path &near_path,
                                  std::filesystem::path &retained,
                                  std::error_code &error) noexcept
{
    retained.clear();
    if (staging.descriptor < 0) {
        error = std::make_error_code(std::errc::invalid_argument);
        return false;
    }

    static std::atomic<std::uint64_t> serial{0};
    const std::filesystem::path parent = near_path.parent_path().empty()
                                             ? std::filesystem::path{"."}
                                             : near_path.parent_path();
    for (std::uint32_t attempt = 0; attempt < 1024U; ++attempt) {
        const std::uint64_t id = serial.fetch_add(1U, std::memory_order_relaxed);
        const auto candidate =
            parent /
            (".infiltrator-retained-" +
             std::to_string(static_cast<long long>(::getpid())) + "-" +
             std::to_string(id));
        if (rename_no_replace_at(staging.descriptor,
                                 "payload",
                                 AT_FDCWD,
                                 candidate.c_str(),
                                 error)) {
            retained = candidate;
            release_staging(staging);
            return true;
        }
        if (error != std::errc::file_exists) {
            return false;
        }
    }
    error = std::make_error_code(std::errc::file_exists);
    return false;
}

'''
text = replace_once(text, marker, retained_helper + marker, 'retained helper insertion')

private_dir_pattern = r'''inline std::filesystem::path create_private_directory\(\n    const std::filesystem::path &public_path,\n    OwnedOutputs &outputs,\n    std::error_code &error\) noexcept\n\{.*?\n\}\n\ninline std::filesystem::path create_private_symlink'''
private_dir_replacement = r'''inline std::filesystem::path create_private_directory(
    const std::filesystem::path &public_path,
    ProtectedStaging &staging,
    OwnedOutputs &outputs,
    std::error_code &error) noexcept
{
    if (!create_protected_staging(public_path.parent_path(), staging, error)) {
        return {};
    }
    if (::mkdirat(staging.descriptor, "payload", 0700) != 0) {
        error = std::error_code(errno, std::generic_category());
        return {};
    }

    const auto private_path = staging.access_root() / "payload";
    if (record_at(staging.descriptor, "payload", private_path, outputs, error)) {
        return private_path;
    }
    (void)::unlinkat(staging.descriptor, "payload", AT_REMOVEDIR);
    return {};
}

inline std::filesystem::path create_private_symlink'''
text = sub_once(text, private_dir_pattern, private_dir_replacement, 'create_private_directory')

private_link_pattern = r'''inline std::filesystem::path create_private_symlink\(\n    const std::filesystem::path &public_path,\n    const std::filesystem::path &target,\n    OwnedOutputs &outputs,\n    std::error_code &error\) noexcept\n\{.*?\n\}\n\ninline bool publish_private_tree'''
private_link_replacement = r'''inline std::filesystem::path create_private_symlink(
    const std::filesystem::path &public_path,
    const std::filesystem::path &target,
    ProtectedStaging &staging,
    OwnedOutputs &outputs,
    std::error_code &error) noexcept
{
    if (!create_protected_staging(public_path.parent_path(), staging, error)) {
        return {};
    }
    if (::symlinkat(target.c_str(), staging.descriptor, "payload") != 0) {
        error = std::error_code(errno, std::generic_category());
        return {};
    }

    const auto private_path = staging.access_root() / "payload";
    if (record_at(staging.descriptor, "payload", private_path, outputs, error)) {
        return private_path;
    }
    (void)::unlinkat(staging.descriptor, "payload", 0);
    return {};
}

inline bool publish_private_tree'''
text = sub_once(text, private_link_pattern, private_link_replacement, 'create_private_symlink')

publish_tree_pattern = r'''inline bool publish_private_tree\(const std::filesystem::path &private_root,\n                                 const std::filesystem::path &public_root,\n                                 OwnedOutputs &outputs,\n                                 std::error_code &error\) noexcept\n\{.*?\n\}\n'''
publish_tree_replacement = r'''inline bool publish_private_tree(ProtectedStaging &staging,
                                 const std::filesystem::path &private_root,
                                 const std::filesystem::path &public_root,
                                 OwnedOutputs &outputs,
                                 std::error_code &error) noexcept
{
    if (!rename_no_replace_at(staging.descriptor,
                              "payload",
                              AT_FDCWD,
                              public_root.c_str(),
                              error)) {
        return false;
    }
    for (OwnedOutput &output : outputs) {
        const auto relative = output.path.lexically_relative(private_root);
        output.path = relative.empty() || relative == "."
                          ? public_root
                          : public_root / relative;
    }
    release_staging(staging);
    return true;
}
'''
text = sub_once(text, publish_tree_pattern, publish_tree_replacement, 'publish_private_tree')

publish_output_pattern = r'''inline bool publish_private_output\(const std::filesystem::path &private_path,\n                                   const std::filesystem::path &public_path,\n                                   OwnedOutput &output,\n                                   std::error_code &error\) noexcept\n\{.*?\n\}\n'''
publish_output_replacement = r'''inline bool publish_private_output(ProtectedStaging &staging,
                                   const std::filesystem::path &private_path,
                                   const std::filesystem::path &public_path,
                                   OwnedOutput &output,
                                   std::error_code &error) noexcept
{
    (void)private_path;
    if (!rename_no_replace_at(staging.descriptor,
                              "payload",
                              AT_FDCWD,
                              public_path.c_str(),
                              error)) {
        return false;
    }
    output.path = public_path;
    release_staging(staging);
    return true;
}
'''
text = sub_once(text, publish_output_pattern, publish_output_replacement, 'publish_private_output')

quarantine_pattern = r'''inline std::filesystem::path quarantine_owned\(\n    const OwnedOutput &output,\n    std::error_code &error,\n    std::filesystem::path \*retained_quarantine = nullptr\) noexcept\n\{.*?\n\}\n\ninline bool destination_is_different_device'''
quarantine_replacement = r'''inline bool quarantine_owned(
    const OwnedOutput &output,
    ProtectedStaging &staging,
    std::filesystem::path &quarantine,
    std::error_code &error,
    std::filesystem::path *retained_quarantine = nullptr) noexcept
{
    quarantine.clear();
    if (retained_quarantine != nullptr) {
        retained_quarantine->clear();
    }
    if (!create_protected_staging(output.path.parent_path(), staging, error)) {
        return false;
    }
    if (!rename_no_replace_at(AT_FDCWD,
                              output.path.c_str(),
                              staging.descriptor,
                              "payload",
                              error)) {
        return false;
    }

    quarantine = staging.access_root() / "payload";
    std::error_code inspect_error;
    const Identity current = identity_for_at(staging.descriptor, "payload", inspect_error);
    if (!inspect_error && current.valid && output.identity.valid &&
        current.device == output.identity.device && current.inode == output.identity.inode) {
        error.clear();
        return true;
    }

    std::error_code restore_error;
    if (rename_no_replace_at(staging.descriptor,
                             "payload",
                             AT_FDCWD,
                             output.path.c_str(),
                             restore_error)) {
        release_staging(staging);
    } else {
        std::filesystem::path retained;
        std::error_code retain_error;
        if (retain_staged_payload(staging, output.path, retained, retain_error)) {
            if (retained_quarantine != nullptr) {
                *retained_quarantine = retained;
            }
        } else if (retained_quarantine != nullptr) {
            *retained_quarantine = staging.container_path / "payload";
        }
    }

    error = inspect_error ? inspect_error
                          : std::make_error_code(std::errc::state_not_recoverable);
    return false;
}

inline bool restore_quarantine(ProtectedStaging &staging,
                               const std::filesystem::path &original,
                               std::error_code &error,
                               std::filesystem::path *retained_quarantine = nullptr) noexcept
{
    if (retained_quarantine != nullptr) {
        retained_quarantine->clear();
    }
    if (rename_no_replace_at(staging.descriptor,
                             "payload",
                             AT_FDCWD,
                             original.c_str(),
                             error)) {
        release_staging(staging);
        return true;
    }

    const std::error_code restore_error = error;
    std::filesystem::path retained;
    std::error_code retain_error;
    if (retain_staged_payload(staging, original, retained, retain_error)) {
        if (retained_quarantine != nullptr) {
            *retained_quarantine = retained;
        }
        error = restore_error;
        return false;
    }
    if (retained_quarantine != nullptr) {
        *retained_quarantine = staging.container_path / "payload";
    }
    error = retain_error ? retain_error : restore_error;
    return false;
}

inline bool destination_is_different_device'''
text = sub_once(text, quarantine_pattern, quarantine_replacement, 'quarantine_owned')

restore_owned_pattern = r'''inline bool restore_owned\(const OwnedOutput &output,\n                          const std::filesystem::path &destination,\n                          std::error_code &error,\n                          std::filesystem::path \*retained_quarantine = nullptr\) noexcept\n\{.*?\n\}\n'''
restore_owned_replacement = r'''inline bool restore_owned(const OwnedOutput &output,
                          const std::filesystem::path &destination,
                          std::error_code &error,
                          std::filesystem::path *retained_quarantine = nullptr) noexcept
{
    if (retained_quarantine != nullptr) {
        retained_quarantine->clear();
    }

    ProtectedStaging staging;
    std::filesystem::path quarantine;
    if (!quarantine_owned(output, staging, quarantine, error, retained_quarantine)) {
        return false;
    }

    if (rename_no_replace_at(staging.descriptor,
                             "payload",
                             AT_FDCWD,
                             destination.c_str(),
                             error)) {
        release_staging(staging);
        return true;
    }

    const std::error_code move_error = error;
    std::error_code restore_error;
    std::filesystem::path retained;
    if (!restore_quarantine(staging, output.path, restore_error, &retained)) {
        if (retained_quarantine != nullptr) {
            *retained_quarantine = retained;
        }
        error = restore_error;
        return false;
    }
    error = move_error;
    return false;
}
'''
text = sub_once(text, restore_owned_pattern, restore_owned_replacement, 'restore_owned')

remove_tree_pattern = r'''inline bool remove_owned_tree\(const OwnedOutput &output,\n                              std::error_code &error,\n                              std::filesystem::path \*retained_quarantine = nullptr\) noexcept\n\{.*?\n\}\n'''
remove_tree_replacement = r'''inline bool remove_owned_tree(const OwnedOutput &output,
                              std::error_code &error,
                              std::filesystem::path *retained_quarantine = nullptr) noexcept
{
    if (retained_quarantine != nullptr) {
        retained_quarantine->clear();
    }

    ProtectedStaging staging;
    std::filesystem::path quarantine;
    if (!quarantine_owned(output, staging, quarantine, error, retained_quarantine)) {
        return false;
    }

    std::filesystem::remove_all(quarantine, error);
    if (!error) {
        release_staging(staging);
        return true;
    }

    const std::error_code remove_error = error;
    std::error_code presence_error;
    const auto status = std::filesystem::symlink_status(quarantine, presence_error);
    const bool remains = !presence_error && status.type() != std::filesystem::file_type::not_found;
    if (!remains) {
        release_staging(staging);
        error = remove_error;
        return false;
    }

    std::error_code restore_error;
    std::filesystem::path retained;
    if (restore_quarantine(staging, output.path, restore_error, &retained)) {
        error = remove_error;
        return false;
    }

    if (retained_quarantine != nullptr) {
        *retained_quarantine = retained;
    }
    error = restore_error ? restore_error : remove_error;
    return false;
}
'''
text = sub_once(text, remove_tree_pattern, remove_tree_replacement, 'remove_owned_tree')

cleanup_pattern = r'''inline bool cleanup_owned\(OwnedOutputs &outputs, std::error_code &first_error\) noexcept\n\{.*?\n\}\n\n// Private copy/link paths'''
cleanup_replacement = r'''inline bool cleanup_owned(OwnedOutputs &outputs, std::error_code &first_error) noexcept
{
    bool complete = true;
    first_error.clear();

    for (auto iterator = outputs.rbegin(); iterator != outputs.rend(); ++iterator) {
        ProtectedStaging staging;
        std::filesystem::path quarantine;
        std::filesystem::path retained;
        std::error_code quarantine_error;
        if (!quarantine_owned(*iterator,
                              staging,
                              quarantine,
                              quarantine_error,
                              &retained)) {
            if (quarantine_error == std::errc::no_such_file_or_directory ||
                quarantine_error == std::errc::not_a_directory) {
                continue;
            }
            if (quarantine_error && !first_error) {
                first_error = quarantine_error;
            }
            complete = false;
            continue;
        }

        std::error_code remove_error;
        const bool removed = std::filesystem::remove(quarantine, remove_error);
        if (removed && !remove_error) {
            release_staging(staging);
            continue;
        }

        std::error_code restore_error;
        std::filesystem::path restore_retained;
        if (!restore_quarantine(staging,
                                iterator->path,
                                restore_error,
                                &restore_retained)) {
            if (restore_error && !first_error) {
                first_error = restore_error;
            }
        }
        if (remove_error && !first_error) {
            first_error = remove_error;
        }
        complete = false;
    }

    return complete;
}

// Private copy/link paths'''
text = sub_once(text, cleanup_pattern, cleanup_replacement, 'cleanup_owned')

header.write_text(text)

transfer = Path('src/core/transfer_operation_engine.cpp')
t = transfer.read_text()
t = replace_once(
    t,
    '    destination_ownership::OwnedOutputs owned;\n\n    if (std::filesystem::is_symlink(source_status)) {',
    '    destination_ownership::OwnedOutputs owned;\n    destination_ownership::ProtectedStaging private_staging;\n\n    if (std::filesystem::is_symlink(source_status)) {',
    'private staging declaration')
t = replace_once(
    t,
    '                destination, target, owned, error);',
    '                destination, target, private_staging, owned, error);',
    'private symlink create call')
t = replace_once(
    t,
    '            (void)destination_ownership::publish_private_output(\n                private_link, destination, owned.front(), error);',
    '            (void)destination_ownership::publish_private_output(\n                private_staging, private_link, destination, owned.front(), error);',
    'private symlink publish call')
t = replace_once(
    t,
    '            destination_ownership::create_private_directory(destination, owned, error);',
    '            destination_ownership::create_private_directory(\n                destination, private_staging, owned, error);',
    'private directory create call')
t = replace_once(
    t,
    '        if (!destination_ownership::publish_private_tree(\n                private_destination, destination, owned, error)) {',
    '        if (!destination_ownership::publish_private_tree(\n                private_staging, private_destination, destination, owned, error)) {',
    'private tree publish call')

old_cross = '''    const std::filesystem::path source_quarantine =
        destination_ownership::quarantine_owned(source_output, error);
    if (source_quarantine.empty()) {
        std::error_code cleanup_error;
        const bool cleaned = destination_ownership::cleanup_owned(copied_outputs, cleanup_error);
        return failure(OperationStatus::VerificationFailure,
                       OperationPhase::Verify,
                       decision.destination,
                       "The source changed before the cross-volume move could commit." +
                           std::string{cleaned ? " The copied destination was removed."
                                               : " Concurrently changed destination content was retained."},
                       !cleaned);
    }
'''
new_cross = '''    destination_ownership::ProtectedStaging source_staging;
    std::filesystem::path source_quarantine;
    retained_source_quarantine.clear();
    if (!destination_ownership::quarantine_owned(source_output,
                                                  source_staging,
                                                  source_quarantine,
                                                  error,
                                                  &retained_source_quarantine)) {
        std::error_code cleanup_error;
        const bool cleaned = destination_ownership::cleanup_owned(copied_outputs, cleanup_error);
        return failure(OperationStatus::VerificationFailure,
                       OperationPhase::Verify,
                       decision.destination,
                       "The source changed before the cross-volume move could commit." +
                           std::string{cleaned ? " The copied destination was removed."
                                               : " Concurrently changed destination content was retained."} +
                           (retained_source_quarantine.empty()
                                ? std::string{}
                                : " Displaced source data was retained safely at “" +
                                      retained_source_quarantine.string() + "”."),
                       !cleaned || !retained_source_quarantine.empty());
    }
'''
t = replace_once(t, old_cross, new_cross, 'cross-volume quarantine')

t = t.replace(
    'destination_ownership::restore_quarantine(\n            source_quarantine, source, restore_error)',
    'destination_ownership::restore_quarantine(\n            source_staging, source, restore_error, &retained_source_quarantine)')

t = replace_once(
    t,
    '                                : " The source was retained safely at “" +\n                                      source_quarantine.string() + "”."),',
    '                                : " The source was retained safely at “" +\n                                      (retained_source_quarantine.empty()\n                                           ? source_quarantine.string()\n                                           : retained_source_quarantine.string()) + "”."),',
    'first retained source message')
t = replace_once(
    t,
    '                                : " The source was retained safely at “" +\n                                      source_quarantine.string() + "”."),',
    '                                : " The source was retained safely at “" +\n                                      (retained_source_quarantine.empty()\n                                           ? source_quarantine.string()\n                                           : retained_source_quarantine.string()) + "”."),',
    'second retained source message')

old_tail = '''    std::filesystem::remove_all(source_quarantine, error);
    if (error) {
        const std::error_code remove_error = error;
        std::error_code restore_error;
        const bool remains = path_present(source_quarantine, restore_error);
        bool restored = false;
        if (!restore_error && remains) {
            restored = destination_ownership::restore_quarantine(
                source_quarantine, source, restore_error);
        }
        return failure(OperationEngine::status_for_error(remove_error),
                       OperationPhase::Execute,
                       decision.destination,
                       "The item was copied, but the original could not be removed: " +
                           remove_error.message() +
                           (restore_error
                                ? " Remaining source data was retained at “" +
                                      source_quarantine.string() + "”: " + restore_error.message()
                                : (restored
                                       ? " Remaining source data was restored to its original name."
                                       : " No remaining source object required restoration.")),
                       true);
    }
'''
new_tail = '''    std::filesystem::remove_all(source_quarantine, error);
    if (error) {
        const std::error_code remove_error = error;
        std::error_code restore_error;
        const bool remains = path_present(source_quarantine, restore_error);
        bool restored = false;
        retained_source_quarantine.clear();
        if (!restore_error && remains) {
            restored = destination_ownership::restore_quarantine(
                source_staging, source, restore_error, &retained_source_quarantine);
        }
        return failure(OperationEngine::status_for_error(remove_error),
                       OperationPhase::Execute,
                       decision.destination,
                       "The item was copied, but the original could not be removed: " +
                           remove_error.message() +
                           (restore_error
                                ? " Remaining source data was retained at “" +
                                      (retained_source_quarantine.empty()
                                           ? source_quarantine.string()
                                           : retained_source_quarantine.string()) +
                                      "”: " + restore_error.message()
                                : (restored
                                       ? " Remaining source data was restored to its original name."
                                       : " No remaining source object required restoration.")),
                       true);
    }
'''
t = replace_once(t, old_tail, new_tail, 'cross-volume removal tail')
transfer.write_text(t)

tests = Path('tests/destination_ownership_test.cpp')
q = tests.read_text()
q = replace_once(
    q,
    '    ownership::OwnedOutputs private_outputs;\n    const auto private_root =\n        ownership::create_private_directory(private_public, private_outputs, error);',
    '    ownership::OwnedOutputs private_outputs;\n    ownership::ProtectedStaging private_staging;\n    const auto private_root =\n        ownership::create_private_directory(\n            private_public, private_staging, private_outputs, error);',
    'private fixture staging')
q = replace_once(
    q,
    '    if (!require(!ownership::publish_private_tree(\n                     private_root, private_public, private_outputs, error) &&',
    '    if (!require(!ownership::publish_private_tree(\n                     private_staging, private_root, private_public, private_outputs, error) &&',
    'private fixture publish')
q = replace_once(
    q,
    '    ownership::OwnedOutputs symlink_outputs;\n    error.clear();\n    const auto private_link = ownership::create_private_symlink(\n        symlink_public, fs::path{"owned-target"}, symlink_outputs, error);',
    '    ownership::OwnedOutputs symlink_outputs;\n    ownership::ProtectedStaging symlink_staging;\n    error.clear();\n    const auto private_link = ownership::create_private_symlink(\n        symlink_public, fs::path{"owned-target"}, symlink_staging, symlink_outputs, error);',
    'symlink fixture staging')
q = replace_once(
    q,
    '    if (!require(!ownership::publish_private_output(\n                     private_link, symlink_public, symlink_outputs.front(), error) &&',
    '    if (!require(!ownership::publish_private_output(\n                     symlink_staging, private_link, symlink_public, symlink_outputs.front(), error) &&',
    'symlink fixture publish')

insertion = r'''

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
'''
needle = '    const auto previous_directory = fs::current_path(error);\n'
q = replace_once(q, needle, insertion + '\n' + needle, 'adversarial test insertion')
tests.write_text(q)
