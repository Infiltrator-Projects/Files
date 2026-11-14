#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Run only on a disposable CI host: all block devices here are loop-backed.
set -euo pipefail
worker=$(realpath "${1:?format worker required}")
provider=$(realpath "${2:?NTFS provider required}")
work=$(realpath "${3:?temporary workspace required}")
disk="$work/disk.img"
mnt="$work/mount"
loop=
owns_system=0
owns_mount=0
cleanup() {
    if (( owns_mount )); then umount "$mnt" || true; fi
    if [[ -n $loop ]]; then losetup -d "$loop" || true; fi
    if (( owns_system )); then
        rm -f /System/Filesystems/NTFS.so
        rmdir /System/Filesystems /System 2>/dev/null || true
    fi
}
trap cleanup EXIT
[[ $(id -u) == 0 && ! -e /System && ! -L /System &&
   ! -e $disk && ! -L $disk && ! -e $mnt && ! -L $mnt ]]
mkdir /System
owns_system=1
mkdir -p /System/Filesystems "$mnt"
install -m 0644 "$provider" /System/Filesystems/NTFS.so
truncate -s 160M "$disk"
printf 'label: gpt\nstart=2048, type=L\n' | sfdisk "$disk" >/dev/null
loop=$(losetup --find --show --partscan "$disk")
partition="${loop}p1"
[[ -b $partition ]]
mkfs.ext4 -q -F "$partition"
major=$(stat -c '%t' "$partition")
minor=$(stat -c '%T' "$partition")
major=$((16#$major)); minor=$((16#$minor))
[[ $(cat "/sys/dev/block/$major:$minor/../removable") == 0 ]]

# Identity mismatch, an entire disk, and a mounted partition must all fail
# before the provider can write. The loop partition is non-removable.
before=$(sha256sum "$disk")
if "$worker" --format ntfs NTFS.so 1 "$partition" "$major" "$((minor+1))" TEST; then exit 1; fi
disk_major=$(stat -c '%t' "$loop"); disk_minor=$(stat -c '%T' "$loop")
if "$worker" --format ntfs NTFS.so 1 "$loop" "$((16#$disk_major))" "$((16#$disk_minor))" TEST; then exit 1; fi
[[ $(sha256sum "$disk") == "$before" ]]
mount -o ro,noload "$partition" "$mnt"
owns_mount=1
before=$(sha256sum "$disk")
if "$worker" --format ntfs NTFS.so 1 "$partition" "$major" "$minor" TEST; then exit 1; fi
[[ $(sha256sum "$disk") == "$before" ]]
umount "$mnt"
owns_mount=0

"$worker" --format ntfs NTFS.so 1 "$partition" "$major" "$minor" 'Internal Test'
[[ $(ntfslabel "$partition") == 'Internal Test' ]]
ntfsfix -n "$partition"
echo 'FILES-NONREMOVABLE-FORMAT-PASS'
