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
# Opening a whole loop disk can cause udev to rescan its partition table;
# let that finish before taking the selection identity used by the UI.
disk_major=$(stat -c '%t' "$loop"); disk_minor=$(stat -c '%T' "$loop")
disk_sequence=$(cat "/sys/dev/block/$((16#$disk_major)):$((16#$disk_minor))/diskseq")
before=$(sha256sum "$disk")
if "$worker" --format ntfs NTFS.so 1 "$loop" "$((16#$disk_major))" "$((16#$disk_minor))" "$disk_sequence" TEST; then exit 1; fi
[[ $(sha256sum "$disk") == "$before" ]]
udevadm settle --timeout=10
major=$(stat -c '%t' "$partition")
minor=$(stat -c '%T' "$partition")
major=$((16#$major)); minor=$((16#$minor))
[[ $(cat "/sys/dev/block/$major:$minor/../removable") == 0 ]]
sequence=$(cat "/sys/dev/block/$major:$minor/../diskseq")
[[ $sequence =~ ^[1-9][0-9]*$ ]]

# Identity mismatch, an entire disk, and a mounted partition must all fail
# before the provider can write. The loop partition is non-removable.
before=$(sha256sum "$disk")
if "$worker" --format ntfs NTFS.so 1 "$partition" "$major" "$((minor+1))" "$sequence" TEST; then exit 1; fi
if "$worker" --format ntfs NTFS.so 1 "$partition" "$major" "$minor" "$((sequence+1))" TEST; then exit 1; fi
[[ $(sha256sum "$disk") == "$before" ]]
mount -o ro,noload "$partition" "$mnt"
owns_mount=1
before=$(sha256sum "$disk")
if "$worker" --format ntfs NTFS.so 1 "$partition" "$major" "$minor" "$sequence" TEST; then exit 1; fi
[[ $(sha256sum "$disk") == "$before" ]]
umount "$mnt"
owns_mount=0

printf 'Expected partition: %s %u:%u\n' "$partition" "$major" "$minor"
stat -c 'Observed partition device: %t:%T' "$partition"
ls -l "/sys/dev/block/$major:$minor/partition"

"$worker" --format ntfs NTFS.so 1 "$partition" "$major" "$minor" "$sequence" 'Internal Test'
[[ $(ntfslabel "$partition") == 'Internal Test' ]]
ntfsfix -n "$partition"
echo 'FILES-NONREMOVABLE-FORMAT-PASS'
