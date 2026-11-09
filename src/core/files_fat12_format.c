// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Privileged, format-neutral block I/O host for the FAT12.so pilot.
 * Contains NO FAT12 parser, allocation, directory or format algorithm.
 * Only Files (unprivileged UI) chooses a volume and asks to unmount it.
 * The host insists on an exclusive partition descriptor and invokes the
 * installed, root-owned filesystem module using Common's versioned ABI.
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <infiltratr/filesystem_module.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct { int fd; uint64_t size; } BlockHost;

static int read_at(void *arg, uint64_t offset, void *buffer, uint32_t bytes)
{
    BlockHost *h=(BlockHost *)arg;
    return h && buffer && offset<=h->size && bytes<=h->size-offset &&
           offset<=INT64_MAX && bytes<=INT64_MAX-offset &&
           pread(h->fd,buffer,bytes,(off_t)offset)==(ssize_t)bytes ? 0 : -1;
}
static int write_at(void *arg, uint64_t offset, const void *buffer, uint32_t bytes)
{
    BlockHost *h=(BlockHost *)arg;
    return h && buffer && offset<=h->size && bytes<=h->size-offset &&
           offset<=INT64_MAX && bytes<=INT64_MAX-offset &&
           pwrite(h->fd,buffer,bytes,(off_t)offset)==(ssize_t)bytes ? 0 : -1;
}
static int flush_at(void *arg)
{
    BlockHost *h=(BlockHost *)arg;
    return h && fsync(h->fd)==0 ? 0 : -1;
}
static int trusted(int fd,int directory)
{
    struct stat st;
    return fstat(fd,&st)==0 && st.st_uid==0u && !(st.st_mode&0022u) &&
           (directory ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode));
}
static int parse_uint(const char *text, unsigned *value)
{
    char *end=NULL;
    unsigned long n;
    if (!text || !*text || *text=='-' || *text=='+') return -1;
    errno=0;
    n=strtoul(text,&end,10);
    if(errno || !end || *end || n>UINT_MAX) return -1;
    *value=(unsigned)n;
    return 0;
}
static int load_module(int *module_fd,void **handle,
                       const IfsFsModuleApiV1 **api)
{
    int system=-1,dir=-1,fd=-1,ok=-1;
    char path[64];
    void *symbol=NULL;
    IfsFsModuleApiEntryV1 entry=NULL;
    system=open("/System",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(system<0 || !trusted(system,1)) goto done;
    dir=openat(system,"Filesystems",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0 || !trusted(dir,1)) goto done;
    fd=openat(dir,"FAT12.so",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0 || !trusted(fd,0)) goto done;
    if(snprintf(path,sizeof path,"/proc/self/fd/%d",fd)>=(int)sizeof path)
        goto done;
    *handle=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if(!*handle) goto done;
    symbol=dlsym(*handle,IFS_FS_COMMON_EXPORT_V1);
    if(!symbol || sizeof(symbol)!=sizeof(entry)) goto done;
    memcpy(&entry,&symbol,sizeof entry);
    *api=entry ? entry():NULL;
    if(!ifs_fs_common_api_valid(*api,"fat12") ||
       !((*api)->capabilities&IFS_FS_CAP_FORMAT) || !(*api)->format ||
       !(*api)->probe) goto done;
    *module_fd=fd;fd=-1;ok=0;
done:
    if(fd>=0) close(fd);
    if(dir>=0) close(dir);
    if(system>=0) close(system);
    if(ok && *handle) {dlclose(*handle);*handle=NULL;}
    return ok;
}
int main(int argc,char **argv)
{
    unsigned wanted_major=0,wanted_minor=0,actual_major=0,actual_minor=0;
    int fd=-1,mod_fd=-1,read_only=0,sector=0,result=1;
    unsigned long long capacity=0;
    struct stat st;
    char partition[120];
    BlockHost block={-1,0};
    IfsFsHostFormatIoV1 io={0};
    const IfsFsModuleApiV1 *api=NULL;
    void *module=NULL;
    uint32_t variant=0u;

    if(argc==2 && strcmp(argv[1],"--help")==0) {
        puts("Usage: pkexec /System/Commands/files-fat12-format "
             "--format-fat12 /dev/PARTITION MAJOR MINOR");
        return 0;
    }
    if(argc!=5 || strcmp(argv[1],"--format-fat12")!=0 ||
       strncmp(argv[2],"/dev/",5)!=0 || !argv[2][5] ||
       strchr(argv[2]+5,'/') || parse_uint(argv[3],&wanted_major)!=0 ||
       parse_uint(argv[4],&wanted_minor)!=0 || geteuid()!=0) {
        fputs("Invalid or unauthorised FAT12 format request\n",stderr);
        return 2;
    }
    fd=open(argv[2],O_RDWR|O_EXCL|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0 || fstat(fd,&st)!=0 || !S_ISBLK(st.st_mode)) {
        fputs("Target is unavailable, mounted, linked or not a block device\n",stderr);
        goto done;
    }
    actual_major=major(st.st_rdev);
    actual_minor=minor(st.st_rdev);
    if(actual_major!=wanted_major || actual_minor!=wanted_minor ||
       snprintf(partition,sizeof partition,
                "/sys/dev/block/%u:%u/partition",actual_major,actual_minor)
                >=(int)sizeof partition || access(partition,F_OK)!=0) {
        fputs("Device identity changed or whole-disk target refused\n",stderr);
        goto done;
    }
    if(ioctl(fd,BLKROGET,&read_only)!=0 || read_only ||
       ioctl(fd,BLKSSZGET,&sector)!=0 ||
       (sector!=512 && sector!=1024 && sector!=2048 && sector!=4096) ||
       ioctl(fd,BLKGETSIZE64,&capacity)!=0 ||
       capacity<(unsigned long long)sector*64u ||
       capacity%(unsigned)sector!=0u) {
        fputs("Device is read-only or has invalid partition geometry\n",stderr);
        goto done;
    }
    block.fd=fd;
    block.size=(uint64_t)capacity;
    if(load_module(&mod_fd,&module,&api)!=0) {
        fputs("Installed FAT12.so failed the trusted Common ABI check\n",stderr);
        goto done;
    }
    io.host.structure_bytes=sizeof io;
    io.host.flags=IFS_FS_IO_EXCLUSIVE;
    io.host.context=&block;
    io.host.volume_bytes=block.size;
    io.host.read_at=read_at;
    io.host.write_at=write_at;
    io.host.flush=flush_at;
    io.logical_sector_bytes=(uint32_t)sector;
    /* V4 inside FAT12.so dry-runs the width and refuses ambiguous/foreign
     * geometry BEFORE writing. This executable never falls back to mkfs. */
    if(api->format(&io.host,12u,NULL)!=IFS_FS_OK ||
       flush_at(&block)!=0 ||
       api->probe(&io.host,&variant)!=IFS_FS_OK || variant!=12u) {
        fputs("FAT12 module reported format or independent probe failure\n",stderr);
        goto done;
    }
    puts("FAT12 partition formatted and verified by FAT12.so");
    result=0;
done:
    if(module) dlclose(module);
    if(mod_fd>=0) close(mod_fd);
    if(fd>=0) close(fd);
    return result;
}
