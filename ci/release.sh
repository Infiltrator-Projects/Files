#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Release authority for Files. GitHub Actions orchestrates
# this script; release identity, immutable asset checks and APT observation live
# here so they are reviewable and testable outside workflow YAML.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
VERSION_FILE="$ROOT/VERSION"
readonly RELEASE_ARCHITECTURE='amd64'
readonly APT_POLL_ATTEMPTS=60
readonly APT_POLL_INTERVAL_SECONDS=15

fail() {
    printf 'release: %s\n' "$*" >&2
    exit 1
}

need_command() {
    command -v "$1" >/dev/null 2>&1 || fail "required command is missing: $1"
}

release_version() {
    local version
    version=$(tr -d '[:space:]' <"$VERSION_FILE")
    [[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] ||
        fail "VERSION is not a release version: $version"
    printf '%s\n' "$version"
}

require_sha() {
    [[ "$1" =~ ^[0-9a-f]{40}$ ]] || fail "invalid commit SHA: $1"
}

load_artifacts() {
    local directory=${1:?artifact directory is required}
    need_command dpkg-deb
    need_command sha256sum

    RELEASE_VERSION=$(release_version)
    RELEASE_DIRECTORY=$(cd -- "$directory" && pwd)
    RELEASE_DEB="files_${RELEASE_VERSION}_${RELEASE_ARCHITECTURE}.deb"
    RELEASE_NATIVE="files-${RELEASE_VERSION}-native-installer.run"
    RELEASE_SUMS='SHA256SUMS'

    cd "$RELEASE_DIRECTORY"
    [[ -f "$RELEASE_DEB" ]] || fail "missing release asset: $RELEASE_DEB"
    [[ -f "$RELEASE_NATIVE" ]] || fail "missing release asset: $RELEASE_NATIVE"
    [[ -x "$RELEASE_NATIVE" ]] || fail "native installer is not executable: $RELEASE_NATIVE"
    [[ -f "$RELEASE_SUMS" ]] || fail "missing release asset: $RELEASE_SUMS"
    sha256sum -c "$RELEASE_SUMS"
    [[ "$(dpkg-deb -f "$RELEASE_DEB" Package)" == 'files' ]] ||
        fail "Debian package identity is not files"
    [[ "$(dpkg-deb -f "$RELEASE_DEB" Version)" == "$RELEASE_VERSION" ]] ||
        fail "Debian package version does not match VERSION"
    [[ "$(dpkg-deb -f "$RELEASE_DEB" Architecture)" == "$RELEASE_ARCHITECTURE" ]] ||
        fail "Debian package architecture does not match release architecture"
}

publish_release() {
    local expected_sha=${1:?expected SHA is required}
    local directory=${2:?artifact directory is required}
    require_sha "$expected_sha"
    : "${GH_TOKEN:?GH_TOKEN is required}"
    : "${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is required}"
    need_command git
    need_command gh
    need_command jq

    [[ "$(git -C "$ROOT" rev-parse HEAD)" == "$expected_sha" ]] ||
        fail "publish attempted from a different commit than the tested release"
    load_artifacts "$directory"

    local tag="v${RELEASE_VERSION}"
    local deb="$RELEASE_DIRECTORY/$RELEASE_DEB"
    local native="$RELEASE_DIRECTORY/$RELEASE_NATIVE"
    local sums="$RELEASE_DIRECTORY/$RELEASE_SUMS"
    local expected_assets published_assets
    expected_assets=$(printf '%s\n%s\n%s\n' "$RELEASE_SUMS" "$RELEASE_DEB" "$RELEASE_NATIVE" | sort)
    git -C "$ROOT" fetch origin --tags
    if git -C "$ROOT" rev-parse -q --verify "refs/tags/$tag^{commit}" >/dev/null; then
        [[ "$(git -C "$ROOT" rev-parse "$tag^{commit}")" == "$expected_sha" ]] ||
            fail "$tag does not identify the qualified source"
    fi
    local state draft
    state=$(mktemp)
    if ! gh release view "$tag" --repo "$GITHUB_REPOSITORY" \
        --json isDraft,isPrerelease,targetCommitish > "$state"; then
        gh release create "$tag" --repo "$GITHUB_REPOSITORY" --draft \
            --target "$expected_sha" --title "Files ${RELEASE_VERSION}" \
            --notes "See the maintained architecture and validation contracts."
        gh release view "$tag" --repo "$GITHUB_REPOSITORY" \
            --json isDraft,isPrerelease,targetCommitish > "$state"
    fi
    [[ "$(jq -r .isPrerelease "$state")" == false ]] || fail "unexpected prerelease"
    [[ "$(jq -r .targetCommitish "$state")" == "$expected_sha" ]] || fail "different draft source"
    draft=$(jq -r .isDraft "$state")
    rm -f "$state"
    if [[ "$draft" == true ]]; then
        gh release upload "$tag" "$deb" "$native" "$sums" --repo "$GITHUB_REPOSITORY" --clobber
    fi
    verify_remote() {
        local destination
        destination=$(mktemp -d)
        gh release download "$tag" --repo "$GITHUB_REPOSITORY" --dir "$destination"
        published_assets=$(find "$destination" -maxdepth 1 -type f -printf '%f\n' | sort)
        [[ "$published_assets" == "$expected_assets" ]] || fail "unexpected release asset set"
        cmp "$deb" "$destination/$RELEASE_DEB"
        cmp "$native" "$destination/$RELEASE_NATIVE"
        cmp "$sums" "$destination/$RELEASE_SUMS"
        rm -rf "$destination"
    }
    verify_remote
    if [[ "$draft" == true ]]; then
        gh release edit "$tag" --repo "$GITHUB_REPOSITORY" --draft=false --latest
    fi
    verify_remote
    git -C "$ROOT" fetch origin --tags
    [[ "$(git -C "$ROOT" rev-list -n 1 "$tag")" == "$expected_sha" ]] ||
        fail "published tag does not identify the tested source"
    printf 'release %s published and verified at %s\n' "$tag" "$expected_sha"

}

wait_for_apt() {
    local directory=${1:?artifact directory is required}
    load_artifacts "$directory"
    need_command awk
    need_command curl
    need_command gzip
    need_command jq

    local catalogue='https://infiltrator-projects.github.io/Infiltrator-Repository/catalogue/apps.json'
    local packages="https://infiltrator-projects.github.io/Infiltrator-Repository/dists/beta/main/binary-${RELEASE_ARCHITECTURE}/Packages.gz"
    local attempt cache_key catalogue_version apt_match

    # The central repository has a five-minute scheduled recovery publisher.
    # Verification therefore needs a materially larger observation window than
    # one scheduler period; otherwise normal GitHub scheduling jitter can turn
    # a successful release into a false-red File Manager workflow.
    for attempt in $(seq 1 "$APT_POLL_ATTEMPTS"); do
        cache_key="${GITHUB_RUN_ID:-manual}-${attempt}"
        catalogue_version=$(curl -fsSL --retry 2 --retry-delay 1 \
            -H 'Cache-Control: no-cache' "${catalogue}?release=${cache_key}" 2>/dev/null \
            | jq -r '.[] | select(.id == "files") | (.version // empty)' \
            | head -n1 || true)

        apt_match=false
        if curl -fsSL --retry 2 --retry-delay 1 \
            -H 'Cache-Control: no-cache' "${packages}?release=${cache_key}" 2>/dev/null \
            | gzip -dc 2>/dev/null \
            | awk -v package='files' -v version="$RELEASE_VERSION" '
                BEGIN { RS=""; FS="\n"; found=0 }
                {
                    p=""; v=""
                    for (i=1; i<=NF; ++i) {
                        if ($i ~ /^Package: /) { p=substr($i,10) }
                        if ($i ~ /^Version: /) { v=substr($i,10) }
                    }
                    if (p == package && v == version) { found=1 }
                }
                END { exit(found ? 0 : 1) }
            '; then
            apt_match=true
        fi

        if [[ "$catalogue_version" == "$RELEASE_VERSION" && "$apt_match" == true ]]; then
            printf 'Files %s is live in the public catalogue and beta APT repository.\n' "$RELEASE_VERSION"
            return 0
        fi

        printf 'APT propagation pending (%d/%d): catalogue=%s package=%s\n' \
            "$attempt" "$APT_POLL_ATTEMPTS" "${catalogue_version:-missing}" "$apt_match"
        sleep "$APT_POLL_INTERVAL_SECONDS"
    done

    fail "APT propagation has not completed within the publication observation window; the verified GitHub release remains valid"
}

usage() {
    cat <<'USAGE'
Usage: ci/release.sh COMMAND [ARGS]

Commands:
  publish EXPECTED_SHA DIRECTORY   Publish/resume the exact tested GitHub release.
  wait-apt DIRECTORY               Verify central catalogue and beta APT publication.
USAGE
}

case "${1:-}" in
    publish)
        shift
        [[ $# -eq 2 ]] || { usage >&2; exit 2; }
        publish_release "$1" "$2"
        ;;
    wait-apt)
        shift
        [[ $# -eq 1 ]] || { usage >&2; exit 2; }
        wait_for_apt "$1"
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac
