#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
DESTINATION=${1:-"$ROOT/dist"}
VERSION=$(tr -d '[:space:]' < "$ROOT/VERSION")
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]

test -f "$ROOT/packaging/native-installer.sh"
test -f "$ROOT/src/infiltratr-common/VERSION"
mkdir -p "$DESTINATION"
DESTINATION=$(cd -- "$DESTINATION" && pwd)

payload=$(mktemp)
trap 'rm -f -- "$payload"' EXIT

tar --exclude-vcs \
    --exclude='./build' \
    --exclude='./dist' \
    --exclude='./.build' \
    --exclude='./.cache' \
    -czf "$payload" -C "$ROOT" .

output="$DESTINATION/files-${VERSION}-native-installer.run"
cat "$ROOT/packaging/native-installer.sh" "$payload" > "$output"
chmod 0755 "$output"

test -s "$output"
printf '%s\n' "$output"
