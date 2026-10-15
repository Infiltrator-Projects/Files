#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Release authority for Infiltrator File Manager. GitHub Actions orchestrates
# this script; release identity, immutable asset checks and APT observation live
# here so they are reviewable and testable outside workflow YAML.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
VERSION_FILE="$ROOT/VERSION"

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
    RELEASE_DEB="infiltrator-file-manager_${RELEASE_VERSION}_amd64.deb"
    RELEASE_SUMS='SHA256SUMS'

    cd "$RELEASE_DIRECTORY"
    [[ -f "$RELEASE_DEB" ]] || fail "missing release asset: $RELEASE_DEB"
    [[ -f "$RELEASE_SUMS" ]] || fail "missing release asset: $RELEASE_SUMS"
    sha256sum -c "$RELEASE_SUMS"
    [[ "$(dpkg-deb -f "$RELEASE_DEB" Package)" == 'infiltrator-file-manager' ]] ||
        fail "Debian package identity is not infiltrator-file-manager"
    [[ "$(dpkg-deb -f "$RELEASE_DEB" Version)" == "$RELEASE_VERSION" ]] ||
        fail "Debian package version does not match VERSION"
}

publish_release() {
    local expected_sha=${1:?expected SHA is required}
    local directory=${2:?artifact directory is required}
    require_sha "$expected_sha"
    : "${GH_TOKEN:?GH_TOKEN is required}"
    : "${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is required}"
    need_command git
    need_command gh

    [[ "$(git -C "$ROOT" rev-parse HEAD)" == "$expected_sha" ]] ||
        fail "publish attempted from a different commit than the tested release"
    load_artifacts "$directory"

    local tag="v${RELEASE_VERSION}"
    local deb="$RELEASE_DIRECTORY/$RELEASE_DEB"
    local sums="$RELEASE_DIRECTORY/$RELEASE_SUMS"
    local expected_assets existing_assets published_assets name asset

    expected_assets=$(printf '%s\n%s\n' "$RELEASE_SUMS" "$RELEASE_DEB" | sort)
    git -C "$ROOT" fetch origin --tags --force

    local tag_exists=false
    if git -C "$ROOT" rev-parse -q --verify "refs/tags/${tag}" >/dev/null; then
        tag_exists=true
        local tag_commit
        tag_commit=$(git -C "$ROOT" rev-list -n 1 "$tag")
        [[ "$tag_commit" == "$expected_sha" ]] ||
            fail "$tag belongs to $tag_commit, not tested release commit $expected_sha"
    fi

    if gh release view "$tag" --repo "$GITHUB_REPOSITORY" >/dev/null 2>&1; then
        [[ "$tag_exists" == true ]] ||
            fail "release $tag exists without its git tag; refusing ambiguous recovery"
        [[ "$(gh release view "$tag" --repo "$GITHUB_REPOSITORY" --json isDraft --jq '.isDraft')" == false ]] ||
            fail "release $tag is still a draft"
        [[ "$(gh release view "$tag" --repo "$GITHUB_REPOSITORY" --json isPrerelease --jq '.isPrerelease')" == false ]] ||
            fail "release $tag is a prerelease"

        existing_assets=$(gh release view "$tag" --repo "$GITHUB_REPOSITORY" --json assets --jq '.assets[].name')
        while IFS= read -r name; do
            [[ -z "$name" ]] && continue
            case "$name" in
                "$RELEASE_SUMS"|"$RELEASE_DEB") ;;
                *) fail "release $tag has unexpected asset $name" ;;
            esac
        done <<<"$existing_assets"

        for asset in "$deb" "$sums"; do
            name=$(basename "$asset")
            if ! grep -Fxq "$name" <<<"$existing_assets"; then
                gh release upload "$tag" "$asset" --repo "$GITHUB_REPOSITORY"
            fi
        done
    elif [[ "$tag_exists" == true ]]; then
        gh release create "$tag" "$deb" "$sums" \
            --repo "$GITHUB_REPOSITORY" \
            --verify-tag \
            --latest \
            --title "Infiltrator File Manager ${RELEASE_VERSION}" \
            --notes "Infiltrator File Manager ${RELEASE_VERSION}. See the repository README for the current implementation surface and release status."
    else
        gh release create "$tag" "$deb" "$sums" \
            --repo "$GITHUB_REPOSITORY" \
            --target "$expected_sha" \
            --latest \
            --title "Infiltrator File Manager ${RELEASE_VERSION}" \
            --notes "Infiltrator File Manager ${RELEASE_VERSION}. See the repository README for the current implementation surface and release status."
    fi

    gh release edit "$tag" --repo "$GITHUB_REPOSITORY" --latest
    git -C "$ROOT" fetch origin --tags --force
    [[ "$(git -C "$ROOT" rev-list -n 1 "$tag")" == "$expected_sha" ]] ||
        fail "published tag no longer identifies the tested release commit"

    published_assets=$(gh release view "$tag" --repo "$GITHUB_REPOSITORY" --json assets --jq '.assets[].name' | sort)
    [[ "$published_assets" == "$expected_assets" ]] ||
        fail "published release $tag does not contain exactly the expected assets"

    # GitHub exposes SHA-256 digests for release assets.  Compare those against
    # the exact tested files rather than assuming a matching asset name is enough.
    for asset in "$deb" "$sums"; do
        name=$(basename "$asset")
        local expected_digest published_digest
        expected_digest="sha256:$(sha256sum "$asset" | awk '{print $1}')"
        published_digest=$(gh release view "$tag" --repo "$GITHUB_REPOSITORY" \
            --json assets --jq ".assets[] | select(.name == \"${name}\") | .digest")
        [[ "$published_digest" == "$expected_digest" ]] ||
            fail "published asset $name digest does not match the tested artifact"
    done

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
    local packages='https://infiltrator-projects.github.io/Infiltrator-Repository/dists/beta/main/binary-amd64/Packages.gz'
    local attempt cache_key catalogue_version apt_match

    for attempt in $(seq 1 20); do
        cache_key="${GITHUB_RUN_ID:-manual}-${attempt}"
        catalogue_version=$(curl -fsSL --retry 2 --retry-delay 1 \
            -H 'Cache-Control: no-cache' "${catalogue}?release=${cache_key}" 2>/dev/null \
            | jq -r '.[] | select(.id == "infiltrator-file-manager") | (.version // empty)' \
            | head -n1 || true)

        apt_match=false
        if curl -fsSL --retry 2 --retry-delay 1 \
            -H 'Cache-Control: no-cache' "${packages}?release=${cache_key}" 2>/dev/null \
            | gzip -dc 2>/dev/null \
            | awk -v package='infiltrator-file-manager' -v version="$RELEASE_VERSION" '
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

        printf 'APT propagation pending (%d/20): catalogue=%s package=%s\n' \
            "$attempt" "${catalogue_version:-missing}" "$apt_match"
        sleep 15
    done

    fail "APT propagation has not completed yet; the verified GitHub release remains valid"
}

usage() {
    cat <<'USAGE'
Usage: ci/release.sh COMMAND [ARGS]

Commands:
  publish EXPECTED_SHA DIRECTORY   Publish/resume the exact tested GitHub release.
  wait-apt DIRECTORY               Observe central catalogue and beta APT propagation.
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
