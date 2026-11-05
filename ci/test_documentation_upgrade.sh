#!/bin/sh
# Test the actual candidate package against its previous nested build record.
set -eu
[ "$(id -u)" -eq 0 ] || { echo 'Run in an isolated root or as root' >&2; exit 1; }
package=$(realpath "$1")
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
old="$work/old"
docs="$old/System/Resources/Documentation/Files"
mkdir -p "$old/DEBIAN" "$docs"
printf 'Package: files\nVersion: 0.0.0\nArchitecture: amd64\nMaintainer: Fixture <fixture@example.invalid>\nDescription: isolated old documentation layout\n' > "$old/DEBIAN/control"
printf 'old build record' > "$docs/BUILD-INFO"
dpkg-deb --root-owner-group --build "$old" "$work/old.deb" >/dev/null
system="$work/system"
dpkg --root "$system" --unpack "$work/old.deb" >/dev/null
printf 'personal document' > "$system/System/Resources/Documentation/Personal.pdf"
dpkg --root "$system" --unpack "$package" >/dev/null
docs="$system/System/Resources/Documentation"
test ! -e "$docs/Files"
test "$(cat "$docs/Personal.pdf")" = 'personal document'
test -s "$docs/Files-BUILD-INFO"
owner=$(dpkg-query --admindir "$system/var/lib/dpkg" --search /System/Resources/Documentation/Files-BUILD-INFO)
test "$owner" = 'files: /System/Resources/Documentation/Files-BUILD-INFO'
echo 'Files flat build record upgrade and old folder removal passed'
