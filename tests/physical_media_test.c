/* SPDX-License-Identifier: GPL-3.0-or-later
 * SD CID acceptance tests: no block devices opened or modified.
 */
#include "../src/core/physical_media.h"
#include <assert.h>
int main(void)
{
    const char *cid="03a1b2c3d4e5f60718293a4b5c6d7e8f";
    assert(strlen(cid)==32u);
    assert(ifm_is_physical_sd_identity("SD",cid));
    assert(ifm_is_physical_sd_identity("SD","03A1B2C3D4E5F60718293A4B5C6D7E8F"));
    assert(!ifm_is_physical_sd_identity("MMC",cid));
    assert(!ifm_is_physical_sd_identity("SDIO",cid));
    assert(!ifm_is_physical_sd_identity("","03A1B2C3D4E5F60718293A4B5C6D7E8F"));
    assert(!ifm_is_physical_sd_identity("SD","00000000000000000000000000000000"));
    assert(!ifm_is_physical_sd_identity("SD","03A1B2C3D4E5F60718293A4B5C6D7E8X"));
    assert(!ifm_is_physical_sd_identity("SD","03A1B2C3D4E5F60718293A4B5C6D7E8"));
    assert(!ifm_is_physical_sd_identity(NULL,cid));
    assert(!ifm_is_physical_sd_identity("SD",NULL));
    puts("FILES-PHYSICAL-SD-SAFETY-PASS");
    return 0;
}
