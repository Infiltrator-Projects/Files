#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Independently qualify release-owned temporal providers without Python.
set -euo pipefail
root=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
catalogue="$root/temporal-providers.json"
command=${1:-}; destination=${2:-}
[[ "$command" == fetch || "$command" == verify || "$command" == install ]] && [[ -n "$destination" ]] || {
    echo 'usage: temporal-packages.sh fetch|verify|install DIRECTORY [--select kind:id]...' >&2
    exit 2
}
shift 2
declare -a requested=()
while (($#)); do
    [[ "$1" == --select && $# -ge 2 ]] || { echo 'Invalid selection arguments' >&2; exit 2; }
    requested+=("$2")
    shift 2
done
for binary in jq curl sha256sum dpkg-deb dpkg; do
    command -v "$binary" >/dev/null || { echo "Missing native prerequisite: $binary" >&2; exit 1; }
done
jq -e '
  length == 54 and
  ([.[].package] | unique | length) == 54 and
  all(.[]; (.repository | test("^(Calendar|Clock)-[A-Za-z0-9-]+$")) and
     (.package == ("infiltrator-" + (.repository | ascii_downcase))) and
     (.kind | type == "string") and (.id | type == "string"))
' "$catalogue" >/dev/null || { echo 'Invalid independent provider catalogue' >&2; exit 1; }

auth=()
token=${GITHUB_TOKEN:-${GH_TOKEN:-}}
[[ -z "$token" ]] || auth=(-H "Authorization: Bearer $token")
request() {
    curl --fail --location --silent --show-error --retry 4 --retry-delay 1 \
         --retry-all-errors --max-time 120 -H 'User-Agent: Infiltrator-native-provider-test' \
         "$@"
}
verify() {
    local records="$destination/providers.json" record expected package file version checksum size field value actual
    [[ -s "$records" ]] || { echo 'No independent provider snapshot' >&2; return 1; }
    jq -e 'length > 0 and ([.[].package] | unique | length) == length' "$records" >/dev/null ||
        { echo 'Duplicate or missing provider identities' >&2; return 1; }
    while IFS= read -r record; do
        package=$(jq -r '.package' <<<"$record")
        expected=$(jq -c --arg p "$package" '.[] | select(.package == $p)' "$catalogue")
        [[ -n "$expected" ]] || { echo "Unknown provider $package" >&2; return 1; }
        jq -n -e --argjson e "$expected" --argjson r "$record" \
            '$e | to_entries | all(.[]; $r[.key] == .value)' >/dev/null ||
            { echo "Provider catalogue mismatch: $package" >&2; return 1; }
        version=$(jq -r '.version' <<<"$record")
        [[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] ||
            { echo "Invalid provider version: $package" >&2; return 1; }
        file="${package}_${version}_amd64.deb"
        [[ "$(jq -r '.filename' <<<"$record")" == "$file" ]] ||
            { echo "Package filename mismatch: $package" >&2; return 1; }
        checksum=$(jq -r '.sha256' <<<"$record")
        size=$(jq -r '.size' <<<"$record")
        [[ "$checksum" =~ ^[0-9a-f]{64}$ && "$size" =~ ^[0-9]+$ && "$size" -gt 0 ]] ||
            { echo "Invalid digest or size: $package" >&2; return 1; }
        [[ -f "$destination/$file" && ! -L "$destination/$file" ]] ||
            { echo "Missing package: $file" >&2; return 1; }
        [[ "$(stat -c '%s' -- "$destination/$file")" == "$size" ]] ||
            { echo "Package size mismatch: $file" >&2; return 1; }
        printf '%s  %s\n' "$checksum" "$destination/$file" | sha256sum --check --status ||
            { echo "Package checksum mismatch: $file" >&2; return 1; }
        for field in Package Version Architecture; do
            case "$field" in Package) value="$package";; Version) value="$version";; Architecture) value=amd64;; esac
            actual=$(dpkg-deb -f "$destination/$file" "$field")
            [[ "$actual" == "$value" ]] ||
                { echo "Package $field mismatch: $file" >&2; return 1; }
        done
    done < <(jq -c '.[]' "$records")
    local recorded actual
    recorded=$(jq -r '.[].filename' "$records" | LC_ALL=C sort)
    actual=$(find "$destination" -maxdepth 1 -type f -name '*.deb' -printf '%f\n' | LC_ALL=C sort)
    [[ "$recorded" == "$actual" ]] ||
        { echo 'Unexpected Debian package in snapshot' >&2; return 1; }
}
fetch_one() {
    local entry=$1 ordinal=$2 temp=$3 repository package minimum_version tag version slug deb run zip url release asset digest bytes file
    repository=$(jq -r '.repository' <<<"$entry")
    package=$(jq -r '.package' <<<"$entry")
    minimum_version=$(jq -r '.minimum_version // "1.0.0"' <<<"$entry")
    release=$(request -H 'Accept: application/vnd.github+json' "${auth[@]}" \
        "https://api.github.com/repos/Infiltrator-Projects/$repository/releases/latest") ||
        { echo "Cannot retrieve release: $repository" >&2; return 1; }
    tag=$(jq -r '.tag_name // ""' <<<"$release")
    [[ "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] &&
        [[ "$(jq -r '.draft' <<<"$release")" == false ]] &&
        [[ "$(jq -r '.prerelease' <<<"$release")" == false ]] ||
        { echo "No stable canonical release: $repository" >&2; return 1; }
    version=${tag#v}
    dpkg --compare-versions "$version" ge "$minimum_version" ||
        { echo "Provider version below baseline: $repository" >&2; return 1; }
    slug=${repository,,}
    deb="${package}_${version}_amd64.deb"
    run="${slug}-${version}-native-installer.run"
    zip="${slug}-${version}-source.zip"
    jq -e --arg deb "$deb" --arg run "$run" --arg zip "$zip" '
      (.assets | length) == 3 and
      ([.assets[].name] | sort) == ([$deb, $run, $zip] | sort)
    ' <<<"$release" >/dev/null ||
        { echo "Release assets mismatch: $repository" >&2; return 1; }
    asset=$(jq -c --arg name "$deb" '.assets[] | select(.name == $name)' <<<"$release")
    digest=$(jq -r '.digest // ""' <<<"$asset")
    bytes=$(jq -r '.size' <<<"$asset")
    url="https://github.com/Infiltrator-Projects/$repository/releases/download/$tag/$deb"
    [[ "$digest" =~ ^sha256:[0-9a-f]{64}$ ]] &&
        [[ "$bytes" =~ ^[0-9]+$ && "$bytes" -gt 0 ]] &&
        [[ "$(jq -r '.browser_download_url' <<<"$asset")" == "$url" ]] ||
        { echo "Invalid signed release metadata: $repository" >&2; return 1; }
    file="$destination/$deb"
    request --output "$file" "$url" ||
        { rm -f -- "$file"; echo "Cannot fetch package: $deb" >&2; return 1; }
    [[ "$(stat -c '%s' -- "$file")" == "$bytes" ]] &&
        printf '%s  %s\n' "${digest#sha256:}" "$file" | sha256sum --check --status ||
        { rm -f -- "$file"; echo "Published package digest mismatch: $deb" >&2; return 1; }
    jq -n --argjson entry "$entry" --arg version "$version" --arg file "$deb" \
        --arg hash "${digest#sha256:}" --argjson size "$bytes" \
        '$entry + {version:$version,filename:$file,sha256:$hash,size:$size}' > "$temp/$(printf '%04d' "$ordinal").json"
    printf 'Verified independent package: %s\n' "$deb"
}
if [[ "$command" == fetch ]]; then
    mkdir -p -- "$destination"
    destination=$(CDPATH='' cd -- "$destination" && pwd)
    [[ -z "$(find "$destination" -mindepth 1 -maxdepth 1 -print -quit)" ]] ||
        { echo 'Fetch destination must be empty' >&2; exit 1; }
    temp=$(mktemp -d)
    trap 'for id in $(jobs -pr); do kill "$id" 2>/dev/null || :; done; rm -rf -- "$temp"' EXIT
    mapfile -t entries < <(jq -c '.[]' "$catalogue")
    declare -a chosen=()
    for entry in "${entries[@]}"; do
        identity=$(jq -r '.kind + ":" + .id' <<<"$entry")
        if (("${#requested[@]}")); then
            match=0
            for selected in "${requested[@]}"; do
                [[ "$selected" != "$identity" ]] || match=1
            done
            ((match)) || continue
        fi
        chosen+=("$entry")
    done
    if (("${#requested[@]}" != 0 && "${#chosen[@]}" != "${#requested[@]}")); then
        echo 'Invalid or duplicate provider selection' >&2; exit 1
    fi
    active=0
    for i in "${!chosen[@]}"; do
        fetch_one "${chosen[$i]}" "$i" "$temp" &
        active=$((active+1))
        if ((active >= 4)); then
            wait -n
            active=$((active-1))
        fi
    done
    wait
    mapfile -t complete < <(find "$temp" -maxdepth 1 -name '*.json' -type f)
    (("${#complete[@]}" == "${#chosen[@]}")) || {
        echo 'Incomplete native provider snapshot; refusing partial catalogue' >&2
        exit 1
    }
    jq -s '.' "$temp"/*.json > "$destination/providers.json"
fi
if [[ "$command" != fetch ]]; then
    destination=$(CDPATH='' cd -- "$destination" && pwd)
fi
verify
if [[ "$command" == install ]]; then
    mapfile -t packages < <(jq -r '.[].filename' "$destination/providers.json")
    paths=()
    for package in "${packages[@]}"; do paths+=("$destination/$package"); done
    apt=(/usr/bin/apt-get -o DPkg::Lock::Timeout=300 install -y --no-install-recommends)
    if ((EUID != 0)); then apt=(/usr/bin/sudo -- "${apt[@]}"); fi
    "${apt[@]}" "${paths[@]}"
fi
printf 'Qualified independent provider snapshot: %s packages\n' "$(jq length "$destination/providers.json")"
