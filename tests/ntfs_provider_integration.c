/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exercise the Files format host with a real NTFS provider and an ordinary
 * disposable file. This test never opens a block device. */
#define main files_volume_format_main
#include "../src/core/files_volume_format.c"
#undef main

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    Formatter formatter = {.fd=-1, .image=dlopen(argv[1], RTLD_NOW|RTLD_LOCAL)};
    if (!formatter.image || formatter_api(&formatter, "ntfs") != 0) {
        const char *error = dlerror();
        fprintf(stderr, "NTFS provider does not offer hosted V2 formatting: %s\n",
                error ? error : "invalid provider capabilities");
        close_formatter(&formatter);
        return 1;
    }
    int fd = open(argv[2], O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC, 0600);
    if (fd < 0 || ftruncate(fd, 128LL*1024*1024) != 0) {
        if (fd >= 0) close(fd);
        close_formatter(&formatter);
        return 1;
    }
    BlockHost block = {.fd=fd, .size=128u*1024u*1024u};
    const IfsFsHostRuntimeV2 io = {
        .abi_version=IFS_FS_HOST_ABI_V2, .structure_bytes=sizeof(io),
        .capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE,
        .volume_bytes=block.size, .logical_sector_bytes=512u,
        .physical_sector_bytes=512u, .maximum_transfer_bytes=65536u,
        .context=&block, .read_at=read_at, .write_at=write_at,
        .flush=flush_at, .allocate=host_alloc, .release=host_free
    };
    int result = format_and_verify(&formatter, &io, 1u, "V2 Dispatch");
    if (close(fd) != 0) result = 1;
    close_formatter(&formatter);
    return result;
}
