// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Format-neutral discovery and block I/O host for installed Common V2 modules.
 * Only Files (unprivileged UI) chooses a volume and asks to unmount it.
 * The host insists on an exclusive partition descriptor and invokes the
 * installed, root-owned filesystem module using Common's versioned ABI.
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <infiltratr/filesystem_module_v2.h>
#include "physical_media.h"
#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
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

typedef struct { int fd; uint64_t size; int write_started; } BlockHost;
typedef struct {
    int fd;
    void *image;
    const char *format_id;
    const IfsFsCoreOpsV2 *core;
    const IfsFsVolumeOpsV2 *volume;
} Formatter;

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
    if(!h || !buffer || offset>h->size || bytes>h->size-offset ||
       offset>INT64_MAX || bytes>INT64_MAX-offset) return -1;
    if(bytes) h->write_started=1;
    return pwrite(h->fd,buffer,bytes,(off_t)offset)==(ssize_t)bytes ? 0 : -1;
}
static void *host_alloc(void *arg,size_t bytes)
{
    (void)arg;
    return malloc(bytes);
}
static void host_free(void *arg,void *p)
{
    (void)arg;
    free(p);
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
    if (!text || *text<'0' || *text>'9') return -1;
    errno=0;
    n=strtoul(text,&end,10);
    if(errno || !end || *end || n>UINT_MAX) return -1;
    *value=(unsigned)n;
    return 0;
}
static int module_directory(void)
{
    int system=open("/System",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(system<0) return -1;
    int dir=trusted(system,1) ?
        openat(system,"Filesystems",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC) : -1;
    close(system);
    if(dir>=0 && !trusted(dir,1)) {close(dir);return -1;}
    return dir;
}

static int module_name_valid(const char *name)
{
    const size_t n=strnlen(name,NAME_MAX+1u);
    return n>3u && n<=NAME_MAX && name[0]!='.' && strcmp(name+n-3u,".so")==0 &&
           strspn(name,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-")==n;
}

static int hosted_elf(int fd)
{
    Elf64_Ehdr h;
    return pread(fd,&h,sizeof h,0)==(ssize_t)sizeof h &&
           memcmp(h.e_ident,ELFMAG,SELFMAG)==0 && h.e_ident[EI_CLASS]==ELFCLASS64 &&
           h.e_ident[EI_DATA]==ELFDATA2LSB && h.e_machine==EM_X86_64 && h.e_type==ET_DYN;
}

static int formatter_api(Formatter *module,const char *expected_id)
{
    IfsFsModuleApiEntryV2 entry=NULL;
    void *symbol=dlsym(module->image,IFS_FS_COMMON_EXPORT_V2);
    _Static_assert(sizeof(symbol)==sizeof(entry),"dlsym pointer representation");
    if(!symbol) return -1;
    memcpy(&entry,&symbol,sizeof(entry));
    const IfsFsModuleApiV2 *api=entry();
    if(!api || !ifs_fs_api_v2_valid(api,expected_id ? expected_id : api->format_id) ||
       !(api->runtime_profiles&IFS_FS_PROFILE_HOSTED)) return -1;
    const IfsFsGroupHeaderV2 *volume=ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    if(!volume || !(volume->capabilities&IFS_FS_VOLUME_FORMAT)) return -1;
    module->format_id=api->format_id;
    module->core=(const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    module->volume=(const IfsFsVolumeOpsV2 *)volume;
    return 0;
}

static void close_formatter(Formatter *module)
{
    if(module->image) dlclose(module->image);
    if(module->fd>=0) close(module->fd);
    *module=(Formatter){.fd=-1};
}

static int load_formatter(int directory,const char *name,const char *expected_id,
                          Formatter *module)
{
    if(!module_name_valid(name)) return -1;
    module->fd=openat(directory,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    char path[64];
    if(module->fd>=0 && trusted(module->fd,0) && hosted_elf(module->fd) &&
       snprintf(path,sizeof(path),"/proc/self/fd/%d",module->fd)<(int)sizeof(path)) {
        module->image=dlopen(path,RTLD_NOW|RTLD_LOCAL);
        if(module->image && formatter_api(module,expected_id)==0) return 0;
    }
    close_formatter(module);
    return -1;
}

static int list_formatters(void)
{
    const int fd=module_directory();
    if(fd<0) return 0;
    DIR *dir=fdopendir(fd);
    if(!dir) {close(fd);return 2;}
    struct dirent *item;
    while((item=readdir(dir))!=NULL) {
        Formatter module={.fd=-1};
        if(load_formatter(fd,item->d_name,NULL,&module)==0) {
            printf("%s\t%s\n",module.format_id,item->d_name);
            close_formatter(&module);
        }
    }
    closedir(dir);
    return 0;
}

static int format_and_verify(const Formatter *module,const IfsFsHostRuntimeV2 *io,
                             uint32_t variant,const char *label)
{
    int status=module->volume->format(io,variant,label,0u);
    if(status!=IFS_FS_OK) {
        fprintf(stderr,"%s formatter returned Common V2 error %d\n",module->format_id,status);
        return 1;
    }
    if(!((const BlockHost *)io->context)->write_started) {
        fprintf(stderr,"%s formatter reported success without writing\n",module->format_id);
        return 1;
    }
    IfsFsHostRuntimeV2 verify=*io;
    verify.capabilities=IFS_FS_HOST_READ_ONLY;
    verify.write_at=NULL;
    verify.flush=NULL;
    uint32_t detected=0u;
    if(io->flush(io->context)!=0 || module->core->probe(&verify,&detected)!=IFS_FS_OK ||
       detected!=variant) {
        fprintf(stderr,"Could not flush or verify the new %s filesystem\n",module->format_id);
        return 1;
    }
    printf("%s partition formatted and verified\n",module->format_id);
    return 0;
}
int main(int argc,char **argv)
{
    unsigned wanted_major=0,wanted_minor=0,actual_major=0,actual_minor=0,variant=0;
    int fd=-1,directory=-1,read_only=0,sector=0,result=2;
    unsigned long long capacity=0;
    struct stat st;
    char partition[120];
    BlockHost block={.fd=-1};
    IfsFsHostRuntimeV2 io={0};
    int physical_sector=0;
    Formatter module={.fd=-1};

    if(argc==2 && strcmp(argv[1],"--help")==0) {
        puts("Usage: files-volume-format --list-formats\n"
             "       pkexec /System/Commands/files-volume-format "
             "--format FORMAT_ID MODULE.so VARIANT /dev/PARTITION MAJOR MINOR [LABEL]");
        return 0;
    }
    if(argc==2 && strcmp(argv[1],"--list-formats")==0) return list_formatters();
    if((argc!=8 && argc!=9) || strcmp(argv[1],"--format")!=0 ||
       !ifs_fs_format_id_valid(argv[2]) || !module_name_valid(argv[3]) ||
       parse_uint(argv[4],&variant)!=0 || strncmp(argv[5],"/dev/",5)!=0 ||
       !argv[5][5] || strchr(argv[5]+5,'/') ||
       parse_uint(argv[6],&wanted_major)!=0 || parse_uint(argv[7],&wanted_minor)!=0 ||
       geteuid()!=0) {
        fputs("Invalid or unauthorised volume format request\n",stderr);
        return 2;
    }
    directory=module_directory();
    if(directory<0 || load_formatter(directory,argv[3],argv[2],&module)!=0) {
        fprintf(stderr,"%s no longer provides trusted Common V2 formatting for %s\n",argv[3],argv[2]);
        goto done;
    }
    fd=open(argv[5],O_RDWR|O_EXCL|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0 || fstat(fd,&st)!=0 || !S_ISBLK(st.st_mode)) {
        fputs("Target is unavailable, mounted, linked or not a block device\n",stderr);
        goto done;
    }
    actual_major=major(st.st_rdev);
    actual_minor=minor(st.st_rdev);
    if(actual_major!=wanted_major || actual_minor!=wanted_minor) {
        fputs("Device identity changed since selection\n",stderr);
        goto done;
    }
    if(snprintf(partition,sizeof partition,
                "/sys/dev/block/%u:%u/partition",actual_major,actual_minor)
                >=(int)sizeof partition || access(partition,F_OK)!=0) {
        fputs("Partition identity is unavailable; whole-disk target refused\n",stderr);
        goto done;
    }
    if(ioctl(fd,BLKROGET,&read_only)!=0 || read_only ||
       ioctl(fd,BLKSSZGET,&sector)!=0 ||
       sector<512 || sector>65536 || (sector&(sector-1)) ||
       ioctl(fd,BLKGETSIZE64,&capacity)!=0 ||
       capacity<(unsigned long long)sector ||
       capacity%(unsigned)sector!=0u) {
        fputs("Device is read-only or has invalid partition geometry\n",stderr);
        goto done;
    }
    block.fd=fd;
    block.size=(uint64_t)capacity;
    if(ioctl(fd,BLKPBSZGET,&physical_sector)!=0 ||
       physical_sector<sector || physical_sector>65536 ||
       (physical_sector&(physical_sector-1))) {
        fputs("Cannot verify physical sector geometry for Common V2\n",stderr);
        goto done;
    }
    io.abi_version=IFS_FS_HOST_ABI_V2;
    io.structure_bytes=sizeof(io);
    io.capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE;
    io.volume_bytes=block.size;
    io.logical_sector_bytes=(uint32_t)sector;
    io.physical_sector_bytes=(uint32_t)physical_sector;
    io.maximum_transfer_bytes=65536u;
    io.context=&block;
    io.read_at=read_at;
    io.write_at=write_at;
    io.flush=flush_at;
    io.allocate=host_alloc;
    io.release=host_free;
    if(!ifs_fs_host_v2_valid(&io)) {
        fputs("Invalid Common V2 host descriptor\n",stderr);
        goto done;
    }
    result=format_and_verify(&module,&io,variant,argc==9 ? argv[8] : "");
    /* A provider may reject a variant, label or geometry before writing. */
    if(result!=0 && !block.write_started) result=2;
done:
    if(fd>=0) close(fd);
    close_formatter(&module);
    if(directory>=0) close(directory);
    return result;
}
