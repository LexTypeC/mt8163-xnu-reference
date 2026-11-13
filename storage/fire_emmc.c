/* Fire MT8163: HFS root read-only, separate bounded userdata-backed data disk.
 * The boot-time controls verify the shared PIO read/write primitives.
 */
#include <pexpert/arm64/board_config.h>
#if defined(FIRE_MT8163)
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <arm/machine_routines.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/systm.h>
#include <sys/uio.h>

extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
/* Declared in osfmk/arm/cpu_data.h, which is not exported to BSD sources. */
extern int get_preemption_level(void);
static vm_offset_t fire_msdc;
struct fire_disk {uint64_t lba,sectors;bool ready,writable;};
static struct fire_disk fire_disks[2];
static bool fire_journal_writing; /* protected by fire_emmc_lock */
#include "fire_cpu_journal.h"
#define FIRE_MSDC_BASE fire_msdc
#define FIRE_MSDC_WRITE 1
#define FIRE_WRITE_FIRST_LBA (fire_journal_writing?FIRE_CPU_JOURNAL_LBA:fire_disks[1].lba)
#define FIRE_WRITE_END_LBA (fire_journal_writing?FIRE_CPU_JOURNAL_LBA+1:fire_disks[1].lba+fire_disks[1].sectors)
#include "fire_msdc_read.h"
#define FIRE_IO_BYTES (128u*1024)
static int fire_bmajor=-1,fire_cmajor=-1;
static _Atomic bool fire_ready;
/* Keep boot-time multi-read acceleration separate from scheduled kernel PIO.
 * Enable kernel batching explicitly while its scheduling behavior is tested. */
static uint32_t fire_bulk_read;
static uint32_t bounce[128];
static uint64_t read_sectors,write_sectors,io_errors;
static uint64_t last_error_ticks,last_error_block;
static uint32_t last_error_stage,last_error_status;
static bool last_error_write;
static LCK_GRP_DECLARE(fire_emmc_group,"fire-emmc");
static LCK_MTX_DECLARE(fire_emmc_lock,&fire_emmc_group);

/* Explicit test only, sleepable context, same serialization as filesystem I/O.
 * No partition-relative arguments or caller-supplied buffers/addresses. The
 * temporary write capability permits exactly the previously reserved sector.
 */
int fire_smp_checkpoint(uint32_t stage,uint32_t cpu,uint64_t value) {
    uint32_t enabled=0,probe=0,run=0;
    PE_parse_boot_argn("fire_smp_journal",&enabled,sizeof(enabled));
    if(!enabled)return 0;
    PE_parse_boot_argn("fire_smp_probe",&probe,sizeof(probe));
    PE_parse_boot_argn("fire_smp_run",&run,sizeof(run));
    if(enabled!=1 || probe!=1 || !run || run>65535 || stage<1 || stage>12 || cpu>3)return EINVAL;
    if(!ml_get_interrupts_enabled() || get_preemption_level())return EBUSY;
    /* BSD may still be publishing its read-only root driver in parallel. */
    uint64_t began=fm_ticks();
    while(!fire_ready){
        if(fm_ticks()-began>65000000)return EBUSY;
        delay_for_interval(1,NSEC_PER_MSEC);
    }
    if(!fire_msdc)return EBUSY;
    static struct fire_cpu_journal journal;
    uint32_t verify[128];int error=0;
    lck_mtx_lock(&fire_emmc_lock);
    if(fire_cpu_journal_append(&journal,run,stage,cpu,value,fm_ticks()))error=EINVAL;
    if(!error){
        fire_journal_writing=true;
        if(!fm_put_sector(FIRE_CPU_JOURNAL_LBA,(const uint32_t *)(const void *)&journal))error=EIO;
        fire_journal_writing=false;
        if(!error && (!fm_sector(FIRE_CPU_JOURNAL_LBA,verify) || memcmp(verify,&journal,512)))error=EIO;
    }
    lck_mtx_unlock(&fire_emmc_lock);
    return error;
}

static struct fire_disk *disk_for(dev_t dev){
    unsigned i=minor(dev);return i<2 && fire_disks[i].ready?&fire_disks[i]:NULL;
}
static int fire_open(dev_t dev,int flags,__unused int type,__unused proc_t p){
    struct fire_disk*d=disk_for(dev);if(!d)return ENXIO;
    return (flags&FWRITE)&&!d->writable?EROFS:0;
}
static int fire_close(__unused dev_t dev,__unused int flags,__unused int type,__unused proc_t p){return 0;}
/* Every caller supplies kernel memory; raw-user copies occur outside this lock. */
static int fire_transfer(struct fire_disk*d,bool write,uint64_t block,uint32_t bytes,char*data,uint32_t*done){
    int error=0;*done=0;
    if(write && !d->writable)return EROFS;
    if(block>d->sectors || bytes>(d->sectors-block)*512)return EINVAL;
    if(write && bytes%512)return EINVAL;
    lck_mtx_lock(&fire_emmc_lock);
    while(*done<bytes){
        unsigned left=bytes-*done,n=left<512?left:512;
        uint32_t lba=(uint32_t)(d->lba+block+*done/512);
        if(!write && left>=512 && !((uintptr_t)(data+*done)&3)){
            unsigned count=fire_bulk_read?left/512:1;if(count>256)count=256;
            if(!fm_read_sectors(lba,count,(uint32_t*)(void*)(data+*done))){error=EIO;break;}
            n=count*512;read_sectors+=count;
        }else if(write){
            const uint32_t*source=(const void*)(data+*done);
            if((uintptr_t)source&3){memcpy(bounce,source,512);source=bounce;}
            if(!fm_put_sector(lba,source)){error=EIO;break;}write_sectors++;
        }else{
            if(!fm_sector(lba,bounce)){error=EIO;break;}
            memcpy(data+*done,bounce,n);read_sectors++;
        }
        *done+=n;
    }
    if(error){
        io_errors++;
        last_error_ticks=fm_ticks();last_error_block=block+*done/512;
        last_error_stage=fm_stage;last_error_status=fm_error;last_error_write=write;
        char line[192];snprintf(line,sizeof(line),"fire-emmc: %s error block%llu stage%u status%#x at_ticks=%llu\n",write?"write":"read",last_error_block,last_error_stage,last_error_status,last_error_ticks);
        extern void loader_log_line(const char *);
        if(io_errors<=16)loader_log_line(line);
        printf("%s",line);
        /* End a failed multi-read before another request can use the card. */
        fm_seen=0;fm_write(FM_ARG,0);fm_write(FM_CMD,12|(6u<<7)|(1u<<14));
        (void)fm_wait(FM_STATUS,3,0);
    }
    lck_mtx_unlock(&fire_emmc_lock);return error;
}
static void fire_strategy(buf_t bp){
    int error=0;uint64_t block=(uint64_t)buf_blkno(bp);
    uint32_t count=buf_count(bp),done=0;caddr_t data=NULL;bool mapped=false;
    bool write=!(buf_flags(bp)&B_READ);struct fire_disk*d=disk_for(buf_device(bp));
    if(!d){error=ENXIO;goto out;}
    if(write && !d->writable){error=EROFS;goto out;}
    if(block>d->sectors){error=EINVAL;goto out;}
    if(count>(d->sectors-block)*512){
        if(write){error=ENOSPC;goto out;}count=(uint32_t)((d->sectors-block)*512);
    }
    if(!count)goto out;
    error=buf_map(bp,&data);if(error)goto out;mapped=true;
    error=fire_transfer(d,write,block,count,data,&done);
out:
    if(mapped)(void)buf_unmap(bp);
    buf_setresid(bp,buf_count(bp)-done);
    if(error)buf_seterror(bp,error);
    buf_biodone(bp);
}
static int fire_ioctl(dev_t dev,u_long cmd,caddr_t data,__unused int flags,__unused proc_t p){
    struct fire_disk*d=disk_for(dev);if(!d)return ENXIO;
    switch(cmd){
    case DKIOCGETBLOCKSIZE:*(uint32_t*)data=512;return 0;
    case DKIOCSETBLOCKSIZE:return *(uint32_t*)data==512?0:EINVAL;
    case DKIOCGETBLOCKCOUNT:*(uint64_t*)data=d->sectors;return 0;
    case DKIOCISWRITABLE:*(uint32_t*)data=d->writable;return 0;
    case DKIOCGETMAXBLOCKCOUNTREAD:
    case DKIOCGETMAXBLOCKCOUNTWRITE:*(uint64_t*)data=256;return 0;
    default:return ENOTTY;
    }
}
static int fire_size(dev_t dev){struct fire_disk*d=disk_for(dev);return d?(int)d->sectors:-1;}
static int fire_dev_rw(dev_t dev,struct uio*uio,bool write){
    struct fire_disk*d=disk_for(dev);if(!d)return ENXIO;
    if(write && !d->writable)return EROFS;
    if(uio_offset(uio)<0 || uio_offset(uio)%512 || uio_resid(uio)%512)return EINVAL;
    char*buffer=kalloc_data(FIRE_IO_BYTES,Z_WAITOK);if(!buffer)return ENOMEM;
    int error=0;
    while(uio_resid(uio)>0){
        uint64_t block=(uint64_t)uio_offset(uio)/512;
        if(block>=d->sectors){error=block==d->sectors&&!write?0:EINVAL;break;}
        uint64_t left=(d->sectors-block)*512;
        uint32_t count=(uint64_t)uio_resid(uio)<left?(uint32_t)uio_resid(uio):(uint32_t)left;
        if(count>FIRE_IO_BYTES)count=FIRE_IO_BYTES;
        if(write){error=uiomove(buffer,count,uio);if(error)break;}
        uint32_t done=0;error=fire_transfer(d,write,block,count,buffer,&done);
        if(!write && done){int copied=uiomove(buffer,done,uio);if(copied)error=copied;}
        if(error || !done)break;
    }
    kfree_data(buffer,FIRE_IO_BYTES);return error;
}
static int fire_read(dev_t dev,struct uio*uio,__unused int flags){return fire_dev_rw(dev,uio,false);}
static int fire_write(dev_t dev,struct uio*uio,__unused int flags){return fire_dev_rw(dev,uio,true);}
static const struct bdevsw fire_bdev={.d_open=fire_open,.d_close=fire_close,.d_strategy=fire_strategy,.d_ioctl=fire_ioctl,.d_dump=eno_dump,.d_psize=fire_size,.d_type=D_DISK};
static const struct cdevsw fire_cdev={.d_open=fire_open,.d_close=fire_close,.d_read=fire_read,.d_write=fire_write,.d_ioctl=fire_ioctl,.d_stop=eno_stop,.d_reset=eno_reset,.d_ttys=NULL,.d_select=eno_select,.d_mmap=eno_mmap,.d_strategy=eno_strat,.d_reserved_1=eno_getc,.d_reserved_2=eno_putc,.d_type=D_DISK};

int fire_emmc_root(dev_t *root);
void fire_emmc_quiesce(void);
void fire_emmc_quiesce(void){
    if(!fire_msdc)return;
    fm_write(FM_INTEN,0);
    if(fm_read(FM_STATUS)&3){
        fm_seen=0;fm_write(FM_ARG,0);fm_write(FM_CMD,12|(6u<<7)|(1u<<14));
        (void)fm_wait(FM_STATUS,3,0);
    }
    (void)fm_wait(0x08,1u<<16,1u<<16);
    /* loader_jump has masked IRQs. printf routes through console_io_allowed,
     * which panics in this context; a nested panic cannot restart the return.
     * Format into stack memory and append via the bounded loader memory log. */
    extern void loader_log_line(const char *);
    char line[160];
    snprintf(line,sizeof(line),"fire-emmc: return reads%llu writes%llu sectors, errors%llu\n",read_sectors,write_sectors,io_errors);
    loader_log_line(line);
    if(io_errors){
        snprintf(line,sizeof(line),"fire-emmc: last error %s block%llu stage%u status%#x at_ticks=%llu\n",last_error_write?"write":"read",last_error_block,last_error_stage,last_error_status,last_error_ticks);
        loader_log_line(line);
    }
}
static int parse_range(DTEntry chosen,unsigned index,const char*name,uint64_t expected_lba,bool required){
    const void*p;unsigned size;
    if(SecureDTGetProperty(chosen,name,&p,&size)!=kSuccess)return required?EINVAL:0;
    if(size!=16)return EINVAL;
    const uint64_t*range=p;
    if(range[0]!=expected_lba || !range[1] || range[1]>2ULL*1024*1024*1024 || range[1]%512)return EINVAL;
    fire_disks[index].lba=range[0];fire_disks[index].sectors=range[1]/512;fire_disks[index].writable=index==1;
    return 0;
}
int fire_emmc_root(dev_t *root){
    if(fire_ready){*root=makedev(fire_bmajor,0);return 0;}
    if(fire_bmajor>=0)return ENXIO;
    DTEntry chosen;
    if(SecureDTLookupEntry(NULL,"/chosen",&chosen)!=kSuccess)return EINVAL;
    PE_parse_boot_argn("fire_bulk_read",&fire_bulk_read,sizeof(fire_bulk_read));
    int error=parse_range(chosen,0,"fire-root-range",8054784,true);if(error)return error;
    error=parse_range(chosen,1,"fire-data-range",12249088,false);if(error)return error;
    fire_msdc=ml_io_map(0x11230000,0x1000);if(!fire_msdc)return ENXIO;
    fm_write(FM_INTEN,0);fm_write(FM_CFG,fm_read(FM_CFG)|8);
    for(unsigned i=0;i<2;i++)if(fire_disks[i].sectors){
        if(!fm_sector((uint32_t)fire_disks[i].lba+2,bounce))return EIO;
        const unsigned char*s=(const void*)bounce;
        if(s[0]!='H' || (s[1]!='+' && s[1]!='X'))return EINVAL;
        fire_disks[i].ready=true;
    }
    fire_bmajor=bdevsw_add(-1,&fire_bdev);if(fire_bmajor<0)return ENXIO;
    fire_cmajor=cdevsw_add_with_bdev(-1,&fire_cdev,fire_bmajor);
    if(fire_cmajor<0){bdevsw_remove(fire_bmajor,&fire_bdev);fire_bmajor=-1;return ENXIO;}
    for(unsigned i=0;i<2;i++)if(fire_disks[i].ready){
        if(!devfs_make_node(makedev(fire_bmajor,i),DEVFS_BLOCK,UID_ROOT,GID_OPERATOR,0640,"disk0s%d",i+1) ||
           !devfs_make_node(makedev(fire_cmajor,i),DEVFS_CHAR,UID_ROOT,GID_OPERATOR,0640,"rdisk0s%d",i+1))return ENXIO;
        printf("fire-emmc: %s disk0s%u LBA%llu sectors%llu\n",fire_disks[i].writable?"writable":"read-only",i+1,fire_disks[i].lba,fire_disks[i].sectors);
    }
    *root=makedev(fire_bmajor,0);fire_ready=true;return 0;
}
#endif
