/* Karnak MSDC0, bounded single-sector PIO reads; opt-in range-limited CMD24.
 * Register/command semantics: pinned mt81xx-sd.c in project analysis cache;
 * RX FIFO port corroborated by the pinned MT6768 mtkloader/msdc.h reference.
 * LK must already have initialized the card; no clocks, power or bus changes.
 */
#define FM_CFG 0x00
#define FM_INT 0x0c
#define FM_INTEN 0x10
#define FM_FIFO 0x14
#define FM_RX 0x1c
#define FM_CMD 0x34
#define FM_ARG 0x38
#define FM_STATUS 0x3c
#define FM_RESP 0x40
#define FM_BLOCKS 0x50
#define FM_BAD ((1u<<9)|(1u<<10)|(1u<<14)|(1u<<15))
static uint32_t fm_seen,fm_error;
static unsigned fm_stage;
#ifndef FIRE_MSDC_BASE
#define FIRE_MSDC_BASE 0x11230000
#endif
static inline uint32_t fm_read(unsigned off){return *(volatile uint32_t*)((uintptr_t)FIRE_MSDC_BASE+off);}
static inline void fm_write(unsigned off,uint32_t value){*(volatile uint32_t*)((uintptr_t)FIRE_MSDC_BASE+off)=value;__asm__ volatile("dsb sy":::"memory");}
static inline uint64_t fm_ticks(void){uint64_t n;__asm__ volatile("mrs %0,cntpct_el0":"=r"(n));return n;}
/* Absolute 2s deadline (13 MHz verified), plus finite iteration bound. */
static int fm_wait(unsigned off,uint32_t mask,uint32_t target){
    uint64_t start=fm_ticks();
    for(unsigned i=0;i<2000000;i++){
        uint32_t v=fm_read(off);
        if(off==FM_INT){fm_seen|=v;fm_write(off,v);if(fm_seen&FM_BAD){fm_error=fm_seen;return 0;}v=fm_seen;}
        if((v&mask)==target)return 1;
        if(fm_ticks()-start>26000000)break;
    }
    fm_error=fm_read(off);return 0;
}
static int fm_read_sectors(uint32_t lba,unsigned blocks,uint32_t *out){
    if(!blocks || blocks>256 || lba+blocks<lba){fm_stage=0;fm_error=blocks;return 0;}
    fm_stage=1;if(!fm_wait(FM_STATUS,3,0))return 0;
    fm_seen=0;fm_write(FM_INT,fm_read(FM_INT));
    fm_write(FM_FIFO,fm_read(FM_FIFO)|(1u<<31));
    fm_stage=2;if(!fm_wait(FM_FIFO,1u<<31,0))return 0;
    fm_write(FM_BLOCKS,blocks);fm_write(FM_ARG,lba);
    /* CMD17 or CMD18/R1/512B, rw bit13 stays zero; manual CMD12 for multi. */
    fm_write(FM_CMD,(blocks==1?17u:18u)|(1u<<7)|((blocks==1?1u:2u)<<11)|(512u<<16));
    fm_stage=3;if(!fm_wait(FM_INT,1u<<8,1u<<8))return 0;
    fm_stage=4;uint64_t start=fm_ticks();unsigned left=128*blocks,spins=0;
    while(left){
        uint32_t status=fm_read(FM_INT);fm_seen|=status;fm_write(FM_INT,status);
        if(fm_seen&FM_BAD){fm_error=fm_seen;return 0;}
        unsigned words=(fm_read(FM_FIFO)&255)/4;if(words>left)words=left;
        for(unsigned i=0;i<words;i++)*out++=fm_read(FM_RX);
        left-=words;
        if(++spins>=2000000 || fm_ticks()-start>26000000){fm_error=left;return 0;}
    }
    fm_stage=5;if(!fm_wait(FM_INT,1u<<12,1u<<12))return 0;
    if(blocks>1){
        fm_stage=7;fm_seen=0;fm_write(FM_INT,fm_read(FM_INT));
        fm_write(FM_ARG,0);fm_write(FM_CMD,12|(6u<<7)|(1u<<14));
        if(!fm_wait(FM_INT,1u<<8,1u<<8))return 0;
    }
    fm_stage=6;return fm_wait(FM_STATUS,3,0);
}
static int fm_sector(uint32_t lba,uint32_t *out){return fm_read_sectors(lba,1,out);}
#ifdef FIRE_MSDC_WRITE
#if !defined(FIRE_WRITE_FIRST_LBA) || !defined(FIRE_WRITE_END_LBA)
#error "PIO writes require an explicit exclusive LBA range"
#endif
/* Caller chooses one bounded disposable range; reads remain unrestricted. */
static __attribute__((noinline)) int fm_put_sector(uint32_t lba,const uint32_t *in){
    if(lba<FIRE_WRITE_FIRST_LBA || lba>=FIRE_WRITE_END_LBA){fm_error=lba;fm_stage=10;return 0;}
    fm_stage=11;if(!fm_wait(FM_STATUS,3,0))return 0;
    fm_seen=0;fm_write(FM_INT,fm_read(FM_INT));
    fm_write(FM_FIFO,fm_read(FM_FIFO)|(1u<<31));
    fm_stage=12;if(!fm_wait(FM_FIFO,1u<<31,0))return 0;
    fm_write(FM_BLOCKS,1);fm_write(FM_ARG,lba);
    /* CMD24/R1/single/512B, write bit13. No mode/erase command. */
    fm_write(FM_CMD,24|(1u<<7)|(1u<<11)|(1u<<13)|(512u<<16));
    fm_stage=13;if(!fm_wait(FM_INT,1u<<8,1u<<8))return 0;
    fm_stage=14;uint64_t start=fm_ticks();unsigned at=0,spins=0;
    while(at<128){
        uint32_t status=fm_read(FM_INT);fm_seen|=status;fm_write(FM_INT,status);
        if(fm_seen&FM_BAD){fm_error=fm_seen;return 0;}
        /* One word into an empty FIFO: no unverified FIFO capacity assumed. */
        if(!(fm_read(FM_FIFO)&0xff0000))fm_write(0x18,in[at++]);
        if(++spins>=2000000 || fm_ticks()-start>26000000){fm_error=at;return 0;}
    }
    fm_stage=15;if(!fm_wait(FM_INT,1u<<12,1u<<12))return 0;
    fm_stage=16;if(!fm_wait(FM_STATUS,3,0))return 0;
    /* DAT0 must be high after card programming (MSDC_PS.DAT bit0). */
    fm_stage=17;return fm_wait(0x08,1u<<16,1u<<16);
}
#endif
