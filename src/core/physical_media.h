/* SPDX-License-Identifier: GPL-3.0-or-later
 * Files volume safety: identify known partitions and removable hardware.
 * A built-in SD reader often exposes /sys/block/mmcblkN/removable as 0.
 * Accept only a removable block device or an MMC subsystem card explicitly
 * identified as SD with a valid non-anonymous card CID. Never accept eMMC.
 * This helper only classifies media: caller separately verifies the partition
 * identity, unmount and exclusive descriptor ownership before writes.
 */
#ifndef INFILTRATOR_FILES_PHYSICAL_MEDIA_H
#define INFILTRATOR_FILES_PHYSICAL_MEDIA_H
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static inline int ifm_is_physical_sd_identity(const char *type,const char *cid)
{
    unsigned seen_nonzero=0u;
    size_t i;
    if (!type || !cid || strcmp(type,"SD")!=0 || strlen(cid)!=32u) return 0;
    for (i=0;i<32u;++i) {
        char c=cid[i];
        if (!((c>='0' && c<='9') || (c>='a' && c<='f') ||
              (c>='A' && c<='F'))) return 0;
        if (c!='0') ++seen_nonzero;
    }
    return seen_nonzero!=0u;
}
static inline int ifm_sysfs_text(const char *path,char *buffer,size_t capacity)
{
    int fd;
    ssize_t bytes;
    if (!path || !buffer || capacity<2u) return 0;
    fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return 0;
    bytes=read(fd,buffer,capacity);
    (void)close(fd);
    if (bytes<=0 || (size_t)bytes>=capacity) return 0;
    buffer[bytes]='\0';
    if (strlen(buffer)!=(size_t)bytes) return 0;
    if (buffer[bytes-1]=='\n') buffer[bytes-1]='\0';
    return 1;
}
/* A known partition is eligible for the UI; this is not write authorization.
 * The privileged host must still open it exclusively and verify its identity. */
static inline int ifm_partition_is_known(unsigned dev_major,unsigned dev_minor)
{
    char path[160],value[32];
    int n=snprintf(path,sizeof path,"/sys/dev/block/%u:%u/partition",dev_major,dev_minor);
    if(n<=0 || (size_t)n>=sizeof path || !ifm_sysfs_text(path,value,sizeof value)) return 0;
    size_t length=strlen(value);
    if(!length || strspn(value,"0123456789")!=length) return 0;
    return strspn(value,"0")!=length;
}
/* Linux assigns each newly attached disk a fresh sequence even if its device
 * number is later reused. Read it before unmounting the selected partition. */
static inline int ifm_partition_disk_sequence(unsigned dev_major,unsigned dev_minor,
                                              uint64_t *sequence)
{
    char path[160],value[32],*end=NULL;
    int n=snprintf(path,sizeof path,"/sys/dev/block/%u:%u/../diskseq",dev_major,dev_minor);
    if(!sequence || n<=0 || (size_t)n>=sizeof path ||
       !ifm_sysfs_text(path,value,sizeof value) || !value[0] ||
       strspn(value,"0123456789")!=strlen(value)) return 0;
    errno=0;
    unsigned long long result=strtoull(value,&end,10);
    if(errno || !end || *end || !result || result>UINT64_MAX) return 0;
    *sequence=(uint64_t)result;
    return 1;
}
static inline int ifm_partition_is_physical_removable(unsigned dev_major,
                                                      unsigned dev_minor)
{
    char path[160],value[64],type[32],cid[64];
    int n=snprintf(path,sizeof path,
                   "/sys/dev/block/%u:%u/../removable",dev_major,dev_minor);
    if (n<=0 || (size_t)n>=sizeof path) return 0;
    if (ifm_sysfs_text(path,value,sizeof value) &&
        strcmp(value,"1")==0) return 1;

    n=snprintf(path,sizeof path,
               "/sys/dev/block/%u:%u/../device/type",dev_major,dev_minor);
    if (n<=0 || (size_t)n>=sizeof path ||
        !ifm_sysfs_text(path,type,sizeof type)) return 0;
    n=snprintf(path,sizeof path,
               "/sys/dev/block/%u:%u/../device/cid",dev_major,dev_minor);
    return n>0 && (size_t)n<sizeof path &&
           ifm_sysfs_text(path,cid,sizeof cid) &&
           ifm_is_physical_sd_identity(type,cid);
}
#endif
