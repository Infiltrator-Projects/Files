/* SPDX-License-Identifier: GPL-3.0-or-later */
#define main files_volume_format_main
#include "../src/core/files_volume_format.c"
#undef main

#define REQUIRE(expr) do { if (!(expr)) { \
    fprintf(stderr,"Line %d: %s\n",__LINE__,#expr); exit(1); \
} } while (0)

static void exercise_formatter(const char *path,const char *id,const char *different_id)
{
    Formatter formatter={.fd=-1,.image=dlopen(path,RTLD_NOW|RTLD_LOCAL)};
    REQUIRE(formatter.image);
    REQUIRE(formatter_api(&formatter,NULL)==0);
    REQUIRE(strcmp(formatter.format_id,id)==0);
    REQUIRE(formatter_api(&formatter,different_id)!=0);
    REQUIRE(formatter_api(&formatter,id)==0);

    FILE *image=tmpfile();
    REQUIRE(image);
    BlockHost block={.fd=fileno(image),.size=65536u};
    REQUIRE(ftruncate(block.fd,(off_t)block.size)==0);
    const IfsFsHostRuntimeV2 io={
        .abi_version=IFS_FS_HOST_ABI_V2,.structure_bytes=sizeof io,
        .capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE,
        .volume_bytes=block.size,.logical_sector_bytes=512u,.physical_sector_bytes=512u,
        .maximum_transfer_bytes=65536u,.context=&block,
        .read_at=read_at,.write_at=write_at,.flush=flush_at,.allocate=host_alloc,.release=host_free
    };
    unsigned char observed[2];
    REQUIRE(format_and_verify(&formatter,&io,0u,"TEST")==1);
    REQUIRE(!block.write_started);
    REQUIRE(read_at(&block,0,observed,sizeof observed)==0 && observed[0]==0u);
    REQUIRE(format_and_verify(&formatter,&io,73u,"TEST")==0);
    REQUIRE(block.write_started);
    REQUIRE(read_at(&block,0,observed,sizeof observed)==0);
    REQUIRE(observed[0]==(unsigned char)id[0] && observed[1]==73u);
    REQUIRE(fclose(image)==0);
    close_formatter(&formatter);
}

int main(int argc,char **argv)
{
    REQUIRE(argc==4);
    REQUIRE(module_name_valid("any-provider.so"));
    REQUIRE(!module_name_valid("../any-provider.so"));
    exercise_formatter(argv[1],"ntfs","custom");
    exercise_formatter(argv[2],"custom","ntfs");

    Formatter probe={.fd=-1,.image=dlopen(argv[3],RTLD_NOW|RTLD_LOCAL)};
    REQUIRE(probe.image);
    REQUIRE(formatter_api(&probe,NULL)!=0);
    close_formatter(&probe);
    return 0;
}
