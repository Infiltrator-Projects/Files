#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -Eeuo pipefail

system_package_mode=0
dry_run=0
for argument in "$@"; do
    case "$argument" in
        --system-package-mode) system_package_mode=1 ;;
        --dry-run) dry_run=1 ;;
        -h|--help)
            printf 'Files hardware-native installer\n\n'
            printf '  --system-package-mode  run as root for InfiltratorOS provisioning\n'
            printf '  --dry-run              validate prerequisites without changing the system\n'
            exit 0
            ;;
        *)
            printf 'Unknown argument: %s\n' "$argument" >&2
            exit 2
            ;;
    esac
done

if ((EUID == 0 && system_package_mode == 0)); then
    printf 'Error: root requires --system-package-mode\n' >&2
    exit 1
fi
if ((EUID != 0 && system_package_mode != 0)); then
    printf 'Error: --system-package-mode requires root\n' >&2
    exit 1
fi

for command in awk tail tar mktemp grep find; do
    command -v "$command" >/dev/null 2>&1 || {
        printf 'Missing required base command: %s\n' "$command" >&2
        exit 1
    }
done

run_root() {
    if ((system_package_mode)); then
        "$@"
    else
        /usr/bin/sudo -- "$@"
    fi
}

missing=0
for command in gcc g++ cmake ninja pkg-config cpack ctest dpkg dpkg-deb; do
    command -v "$command" >/dev/null 2>&1 || missing=1
done
if command -v pkg-config >/dev/null 2>&1; then
    pkg-config --exists 'gtk4 >= 4.10' >/dev/null 2>&1 || missing=1
    # FindLibArchive needs the development header and linker library, not just
    # the runtime .so. The libarchive pkg-config file comes from libarchive-dev.
    # Without this check Mint can skip apt provisioning and fail at CMake.
    pkg-config --exists libarchive >/dev/null 2>&1 || missing=1
fi
command -v xvfb-run >/dev/null 2>&1 || missing=1
command -v dbus-run-session >/dev/null 2>&1 || missing=1

if ((dry_run)); then
    if ((missing)); then
        printf 'Files native installer: build dependencies would be installed.\n'
    else
        printf 'Files native installer: prerequisites are already satisfied.\n'
    fi
    exit 0
fi

if ((system_package_mode == 0)) && [[ ! -x /usr/bin/sudo ]]; then
    printf 'Files native installer requires sudo for a user-mode installation.\n' >&2
    exit 1
fi
if [[ ! -x /usr/bin/apt-get ]]; then
    printf 'Files native installer requires apt-get on Debian/Ubuntu/Mint systems.\n' >&2
    exit 1
fi
if [[ "$(dpkg --print-architecture)" != amd64 ]]; then
    printf 'Files native installer currently supports amd64 only.\n' >&2
    exit 1
fi

if ((missing)); then
    printf 'Installing Files native-build requirements...\n'
    run_root /usr/bin/apt-get -o DPkg::Lock::Timeout=300 update
    run_root /usr/bin/apt-get -o DPkg::Lock::Timeout=300 install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config libgtk-4-dev libarchive-dev dpkg-dev \
        xvfb xauth dbus-x11
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/files-native-XXXXXX")
trap 'rm -rf -- "$work"' EXIT
source_root="$work/source"
build_root="$work/build"
profile_root="$work/profile"
dist_root="$work/dist"
mkdir -p "$source_root" "$profile_root" "$dist_root"

payload_line=$(awk '/^__FILES_PAYLOAD_BELOW__$/ { print NR + 1; exit }' "$0")
if [[ -z "$payload_line" ]]; then
    printf 'Files native installer payload marker is missing.\n' >&2
    exit 1
fi
tail -n +"$payload_line" "$0" | tar -xzf - -C "$source_root"

version=$(tr -d '[:space:]' < "$source_root/VERSION")
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
    printf 'Invalid Files VERSION in installer payload.\n' >&2
    exit 1
}
native_version="${version}+native1"

# Version ordering must express the difference between the generic APT build
# and this locally trained PGO build. Never silently downgrade a newer release.
installed_version=''
if [[ "$(dpkg-query -W -f='${db:Status-Abbrev}' files 2>/dev/null || true)" == ii* ]]; then
    installed_version=$(dpkg-query -W -f='${Version}' files)
fi
if [[ -n "$installed_version" ]] &&
   dpkg --compare-versions "$installed_version" gt "$native_version"; then
    printf 'Refusing to downgrade Files from %s to %s.\n' \
        "$installed_version" "$native_version" >&2
    exit 1
fi

# Mirror System Monitor's proven ELF call/visibility optimisations when both
# installed C and C++ compilers accept them. Native CPU/LTO/PGO remain mandatory.
base_flags='-O3 -DNDEBUG -march=native -mtune=native -flto=auto'
for flag in -fno-semantic-interposition -fno-plt; do
    if printf 'int main(void){return 0;}\n' |
           gcc -x c -c "$flag" -o /dev/null - >/dev/null 2>&1 &&
       printf 'int main(){return 0;}\n' |
           g++ -x c++ -c "$flag" -o /dev/null - >/dev/null 2>&1; then
        base_flags+=" $flag"
    fi
done
generate_flags="$base_flags -fprofile-generate=$profile_root -fprofile-update=atomic"
use_flags="$base_flags -fprofile-use=$profile_root -fprofile-correction -fprofile-partial-training -Wno-missing-profile -Wno-error=missing-profile"

printf 'Files %s: PGO training build for this CPU...\n' "$version"
CC=gcc CXX=g++ cmake -S "$source_root" -B "$build_root" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_RELEASE="$generate_flags" \
    -DCMAKE_CXX_FLAGS_RELEASE="$generate_flags" \
    -DINFILTRATOR_BUILD_PROFILE=aggressive-training \
    -DINFILTRATOR_PGO_STATUS='profile generation pass' \
    -DBUILD_TESTING=ON
cmake --build "$build_root" --parallel

# Native PGO trains only explicitly labelled, deterministic core workloads.
# Never run UI, desktop-portal, session-bus, display, or mounted-device tests
# during a customer's installation. The complete unfiltered test suite is a
# mandatory release CI gate instead. An allowlist prevents future GTK tests
# from silently entering the native installation workload.
printf 'Training Files native PGO with isolated core workloads...\n'
ctest --test-dir "$build_root" --output-on-failure -L '^native-pgo

printf 'Files %s: final aggressive native PGO build...\n' "$version"
rm -rf -- "$build_root"
CC=gcc CXX=g++ cmake -S "$source_root" -B "$build_root" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_RELEASE="$use_flags" \
    -DCMAKE_CXX_FLAGS_RELEASE="$use_flags" \
    -DINFILTRATOR_BUILD_PROFILE=aggressive \
    -DINFILTRATOR_PGO_STATUS='trained two-pass profile' \
    -DINFILTRATOR_NATIVE_PACKAGE=ON \
    -DBUILD_TESTING=OFF
cmake --build "$build_root" --parallel
cpack --config "$build_root/CPackConfig.cmake" -G DEB -B "$dist_root"

mapfile -t packages < <(find "$dist_root" -maxdepth 1 -type f -name '*.deb' -print)
if [[ ${#packages[@]} -ne 1 ]]; then
    printf 'Expected exactly one native Files package, found %s.\n' "${#packages[@]}" >&2
    exit 1
fi
deb=${packages[0]}

[[ "$(dpkg-deb -f "$deb" Package)" == files ]]
[[ "$(dpkg-deb -f "$deb" Version)" == "$native_version" ]]
[[ "$(dpkg-deb -f "$deb" Architecture)" == amd64 ]]

inspect="$work/package"
dpkg-deb -x "$deb" "$inspect"
test -x "$inspect/Programs/files"
test ! -e "$inspect/usr/bin/files"
test -f "$inspect/usr/share/applications/files.desktop"
grep -Fxq 'Exec=/Programs/files %U' "$inspect/usr/share/applications/files.desktop"
grep -Fxq 'MimeType=inode/directory;' "$inspect/usr/share/applications/files.desktop"
test -f "$inspect/System/Resources/Documentation/Files/BUILD-INFO"
test -x "$inspect/System/Commands/files-ensure-fonts"
test ! -e "$inspect/usr/lib/files"
test ! -e "$inspect/usr/share/doc/files"
grep -Fxq 'Profile: aggressive' "$inspect/System/Resources/Documentation/Files/BUILD-INFO"
grep -Fxq 'PGO: trained two-pass profile' "$inspect/System/Resources/Documentation/Files/BUILD-INFO"
grep -Fq -- '-O3' "$inspect/System/Resources/Documentation/Files/BUILD-INFO"
grep -Fq -- '-march=native' "$inspect/System/Resources/Documentation/Files/BUILD-INFO"

printf 'Installing Files %s into /Programs...\n' "$native_version"
# OS-native directories can be reset independently of Debian's dpkg state.
# If the same version is recorded as installed, a normal apt-get install is a
# no-op and will NOT restore missing /Programs or /System package-owned files.
# Always reinstall the exact locally built package, even on a same-version run.
run_root /usr/bin/apt-get -o DPkg::Lock::Timeout=300 install -y --reinstall "$deb"

[[ "$(dpkg-query -W -f='${Version}' files)" == "$native_version" ]]
test -x /Programs/files
test ! -e /usr/bin/files
grep -Fxq 'Exec=/Programs/files %U' /usr/share/applications/files.desktop
grep -Fxq 'MimeType=inode/directory;' /usr/share/applications/files.desktop
grep -Fxq 'Profile: aggressive' /System/Resources/Documentation/Files/BUILD-INFO
grep -Fxq 'PGO: trained two-pass profile' /System/Resources/Documentation/Files/BUILD-INFO

printf 'Files %s native installation complete: /Programs/files\n' "$native_version"
exit 0

__FILES_PAYLOAD_BELOW__


# Refuse to claim a trained native build if tests were accidentally unlabelled
# or the instrumented executables produced no compiler profile data.
if [[ -z "$(find "$profile_root" -type f -name '*.gcda' -print -quit)" ]]; then
    printf 'Files PGO training produced no GCC profile data; refusing an untrained build.\n' >&2
    exit 1
fi

printf 'Files %s: final aggressive native PGO build...\n' "$version"
rm -rf -- "$build_root"
CC=gcc CXX=g++ cmake -S "$source_root" -B "$build_root" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_RELEASE="$use_flags" \
    -DCMAKE_CXX_FLAGS_RELEASE="$use_flags" \
    -DINFILTRATOR_BUILD_PROFILE=aggressive \
    -DINFILTRATOR_PGO_STATUS='trained two-pass profile' \
    -DINFILTRATOR_NATIVE_PACKAGE=ON \
    -DBUILD_TESTING=OFF
cmake --build "$build_root" --parallel
cpack --config "$build_root/CPackConfig.cmake" -G DEB -B "$dist_root"

mapfile -t packages < <(find "$dist_root" -maxdepth 1 -type f -name '*.deb' -print)
if [[ ${#packages[@]} -ne 1 ]]; then
    printf 'Expected exactly one native Files package, found %s.\n' "${#packages[@]}" >&2
    exit 1
fi
deb=${packages[0]}

[[ "$(dpkg-deb -f "$deb" Package)" == files ]]
[[ "$(dpkg-deb -f "$deb" Version)" == "$native_version" ]]
[[ "$(dpkg-deb -f "$deb" Architecture)" == amd64 ]]

inspect="$work/package"
dpkg-deb -x "$deb" "$inspect"
test -x "$inspect/Programs/files"
test ! -e "$inspect/usr/bin/files"
test -f "$inspect/usr/share/applications/files.desktop"
grep -Fxq 'Exec=/Programs/files %U' "$inspect/usr/share/applications/files.desktop"
grep -Fxq 'MimeType=inode/directory;' "$inspect/usr/share/applications/files.desktop"
test -f "$inspect/System/Resources/Documentation/Files/BUILD-INFO"
test -x "$inspect/System/Commands/files-ensure-fonts"
test ! -e "$inspect/usr/lib/files"
test ! -e "$inspect/usr/share/doc/files"
grep -Fxq 'Profile: aggressive' "$inspect/System/Resources/Documentation/Files/BUILD-INFO"
grep -Fxq 'PGO: trained two-pass profile' "$inspect/System/Resources/Documentation/Files/BUILD-INFO"
grep -Fq -- '-O3' "$inspect/System/Resources/Documentation/Files/BUILD-INFO"
grep -Fq -- '-march=native' "$inspect/System/Resources/Documentation/Files/BUILD-INFO"

printf 'Installing Files %s into /Programs...\n' "$native_version"
# OS-native directories can be reset independently of Debian's dpkg state.
# If the same version is recorded as installed, a normal apt-get install is a
# no-op and will NOT restore missing /Programs or /System package-owned files.
# Always reinstall the exact locally built package, even on a same-version run.
run_root /usr/bin/apt-get -o DPkg::Lock::Timeout=300 install -y --reinstall "$deb"

[[ "$(dpkg-query -W -f='${Version}' files)" == "$native_version" ]]
test -x /Programs/files
test ! -e /usr/bin/files
grep -Fxq 'Exec=/Programs/files %U' /usr/share/applications/files.desktop
grep -Fxq 'MimeType=inode/directory;' /usr/share/applications/files.desktop
grep -Fxq 'Profile: aggressive' /System/Resources/Documentation/Files/BUILD-INFO
grep -Fxq 'PGO: trained two-pass profile' /System/Resources/Documentation/Files/BUILD-INFO

printf 'Files %s native installation complete: /Programs/files\n' "$native_version"
exit 0

__FILES_PAYLOAD_BELOW__
