/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <infiltratr/filesystem_module_v2.h>
#include <string.h>

static int probe(const IfsFsHostRuntimeV2 *host,uint32_t *variant)
{
    unsigned char sector[512];
    unsigned char entropy[8];
    if(!ifs_fs_host_v2_valid(host) || !(host->capabilities&IFS_FS_HOST_READ_ONLY) ||
       !(host->capabilities&IFS_FS_HOST_CLOCK) || !host->monotonic_ns(host->context) ||
       !host->realtime_ns(host->context) ||
       ifs_fs_host_random_bytes_v2(host,entropy,sizeof entropy)!=IFS_FS_OK ||
       host->read_at(host->context,0,sector,sizeof sector)!=0)
        return IFS_FS_IO_ERROR;
    if(sector[0]!=(unsigned char)FORMAT_ID[0]) return IFS_FS_NOT_FOUND;
    *variant=sector[1];
    return IFS_FS_OK;
}

static const IfsFsCoreOpsV2 core={
    .header={IFS_FS_GROUP_ABI_V2,sizeof core,IFS_FS_GROUP_CORE,0,IFS_FS_CORE_PROBE},
    .probe=probe
};

#ifndef PROBE_ONLY
static int format(const IfsFsHostRuntimeV2 *host,uint32_t variant,const char *label,uint32_t flags)
{
    unsigned char sector[512]={(unsigned char)FORMAT_ID[0],(unsigned char)variant};
    unsigned char entropy[8];
    if(!ifs_fs_host_v2_valid(host) || !(host->capabilities&IFS_FS_HOST_EXCLUSIVE) ||
       !(host->capabilities&IFS_FS_HOST_CLOCK) || !host->monotonic_ns(host->context) ||
       !host->realtime_ns(host->context) ||
       ifs_fs_host_random_bytes_v2(host,entropy,sizeof entropy)!=IFS_FS_OK ||
       variant!=73u || !label || strcmp(label,"TEST")!=0 || flags) return IFS_FS_INVALID;
    return host->write_at(host->context,0,sector,sizeof sector)==0 ? IFS_FS_OK : IFS_FS_IO_ERROR;
}

static const IfsFsVolumeOpsV2 volume={
    .header={IFS_FS_GROUP_ABI_V2,sizeof volume,IFS_FS_GROUP_VOLUME,0,IFS_FS_VOLUME_FORMAT},
    .format=format
};
#define EXTRA_GROUP IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME)
#else
#define EXTRA_GROUP 0u
#endif

static const IfsFsGroupHeaderV2 *group(uint32_t id,uint32_t minimum,uint32_t maximum)
{
    if(minimum>IFS_FS_GROUP_ABI_V2 || maximum<IFS_FS_GROUP_ABI_V2) return NULL;
    if(id==IFS_FS_GROUP_CORE) return &core.header;
#ifndef PROBE_ONLY
    if(id==IFS_FS_GROUP_VOLUME) return &volume.header;
#endif
    return NULL;
}

const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void)
{
    static const IfsFsModuleApiV2 api={
        IFS_FS_COMMON_ABI_V2,sizeof api,FORMAT_ID,IFS_FS_PROFILE_HOSTED,
        IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|EXTRA_GROUP,group
    };
    return &api;
}
