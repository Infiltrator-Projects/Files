/* SPDX-License-Identifier: GPL-3.0-or-later
 * SD CID acceptance tests: no block devices opened or modified.
 */
#define _GNU_SOURCE
#include "../src/core/physical_media.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL: %s\n", #x); return 1; } } while (0)
int main(void)
{
    const char *cid="03a1b2c3d4e5f60718293a4b5c6d7e8f";
    CHECK(strlen(cid)==32u);
    CHECK(ifm_is_physical_sd_identity("SD",cid));
    CHECK(ifm_is_physical_sd_identity("SD","03A1B2C3D4E5F60718293A4B5C6D7E8F"));
    CHECK(!ifm_is_physical_sd_identity("MMC",cid));
    CHECK(!ifm_is_physical_sd_identity("SDIO",cid));
    CHECK(!ifm_is_physical_sd_identity("","03A1B2C3D4E5F60718293A4B5C6D7E8F"));
    CHECK(!ifm_is_physical_sd_identity("SD","00000000000000000000000000000000"));
    CHECK(!ifm_is_physical_sd_identity("SD","03A1B2C3D4E5F60718293A4B5C6D7E8X"));
    CHECK(!ifm_is_physical_sd_identity("SD","03A1B2C3D4E5F60718293A4B5C6D7E8"));
    CHECK(!ifm_is_physical_sd_identity(NULL,cid));
    CHECK(!ifm_is_physical_sd_identity("SD",NULL));
    CHECK(!ifm_partition_is_known(0u,0u));
    uint64_t sequence=0;
    CHECK(!ifm_partition_disk_sequence(0u,0u,&sequence));
    CHECK(!ifm_partition_disk_sequence(0u,0u,NULL));
    puts("FILES-PHYSICAL-SD-SAFETY-PASS");
    return 0;
}
