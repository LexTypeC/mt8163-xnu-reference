/* Karnak touch bring-up. Opt-in register probe and bounded identity read.
 * Pinned Linux i2c-mt65xx.c/clk-mt8163.c and captured live DT define registers.
 */
#include <pexpert/arm64/board_config.h>
#if defined(FIRE_MT8163)
#include <mach/mach_types.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include "fire_measure.h"

extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern void loader_log_line(const char *);
extern kern_return_t thread_terminate(thread_t);
void fire_touch_probe(void);
static bool fire_touch_probed;
static vm_offset_t ft_live_i2c,ft_live_dma;
static volatile bool ft_stopping;
void fire_touch_quiesce(void);
static uint32_t ft_read32(vm_offset_t b,unsigned o){return *(volatile uint32_t*)(b+o);}
static uint16_t ft_read16(vm_offset_t b,unsigned o){return *(volatile uint16_t*)(b+o);}
static void ft_write32(vm_offset_t b,unsigned o,uint32_t v){
    *(volatile uint32_t*)(b+o)=v;__asm__ volatile("dsb sy":::"memory");
}
static void ft_write16(vm_offset_t b,unsigned o,uint16_t v){
    *(volatile uint16_t*)(b+o)=v;__asm__ volatile("dsb sy":::"memory");
}
extern uint64_t kvtophys(vm_offset_t);
static unsigned char ft_tx[128] __attribute__((aligned(128)));
static unsigned char ft_rx[128] __attribute__((aligned(128)));
static uint64_t ft_ticks(void){uint64_t n;__asm__ volatile("mrs %0,cntpct_el0":"=r"(n));return n;}
static void ft_cache(bool before){
    for(unsigned i=0;i<128;i+=64){
        if(before){
            __asm__ volatile("dc civac, %0"::"r"(ft_tx+i):"memory");
            __asm__ volatile("dc civac, %0"::"r"(ft_rx+i):"memory");
        }else __asm__ volatile("dc ivac, %0"::"r"(ft_rx+i):"memory");
    }
    __asm__ volatile("dsb sy":::"memory");
}
static void ft_delay(unsigned us){uint64_t start=ft_ticks();for(unsigned i=0;i<20000000 && ft_ticks()-start<(uint64_t)us*13;i++)__asm__ volatile("nop");}
static void ft_pause(void){ft_delay(50);}
/* Status polling is bounded to100ms at the physically verified13MHz counter. */
static uint16_t ft_wait_event(vm_offset_t i2c,uint16_t want){
    uint64_t start=ft_ticks();
    for(unsigned i=0;i<2000000;i++){
        uint16_t s=ft_read16(i2c,0x0c);
        if(s&6 || s&want)return s;
        if(ft_ticks()-start>=1300000)break;
    }
    return 0;
}
static int ft_wrap_wait(vm_offset_t wrap,unsigned want,uint32_t *word){
    uint64_t start=ft_ticks();
    for(unsigned i=0;i<2000000;i++){
        uint32_t v=ft_read32(wrap,0xa4);unsigned state=(v>>16)&7;
        if(!(v&(1u<<21)))return 0;
        if(state==want){*word=v;return 1;}
        if(want==0 && state==6)ft_write32(wrap,0xa8,1);
        if(ft_ticks()-start>=130000)break;
    }
    return 0;
}
/* Read-only whitelist includes chip ID, touch rails and power-key status. */
static __attribute__((noinline)) int ft_pmic_read(vm_offset_t wrap,unsigned reg,uint16_t *value){
    if(reg!=0x100 && reg!=0x142 && reg!=0x50c && reg!=0x532 && reg!=0x536 && reg!=0x538)return 0;
    uint32_t word;
    if(!ft_wrap_wait(wrap,0,&word))return 0;
    ft_write32(wrap,0xa0,(reg>>1)<<16);
    if(!ft_wrap_wait(wrap,6,&word))return 0;
    *value=(uint16_t)word;ft_write32(wrap,0xa8,1);return 1;
}
static void ft_power_snapshot(void){
    vm_offset_t wrap=ml_io_map(0x1000d000,0x1000),gpio=ml_io_map(0x10005000,0x1000);
    if(!wrap || !gpio){loader_log_line("fire-touch: power snapshot mapping failed\n");return;}
    char line[160];
    snprintf(line,sizeof(line),"fire-touch: pwrap WACS2_EN=%#x status=%#x\n",ft_read32(wrap,0x98),ft_read32(wrap,0xa4));loader_log_line(line);
    const unsigned regs[]={0x100,0x50c,0x532,0x536,0x538};
    for(unsigned i=0;i<sizeof(regs)/sizeof(regs[0]);i++){
        uint16_t v=0;int ok=ft_pmic_read(wrap,regs[i],&v);
        snprintf(line,sizeof(line),"fire-touch: pmic reg%#x ok=%u value=%#x\n",regs[i],ok,v);loader_log_line(line);
        if(!ok)break;
    }
    const unsigned pins[]={22,45,77,78};
    for(unsigned i=0;i<sizeof(pins)/sizeof(pins[0]);i++){
        unsigned pin=pins[i],port=(pin/16)*16,bit=pin%16;
        unsigned mode=(ft_read32(gpio,0x600+(pin/5)*16)>>(3*(pin%5)))&7;
        snprintf(line,sizeof(line),"fire-touch: gpio%u mode%u dir%u out%u in%u\n",pin,mode,
            (ft_read32(gpio,port)>>bit)&1,(ft_read32(gpio,0x400+port)>>bit)&1,(ft_read32(gpio,0x500+port)>>bit)&1);loader_log_line(line);
    }
}
static bool ft_transfer(vm_offset_t i2c,vm_offset_t dma,unsigned reg,unsigned length,bool write,uint16_t events[2]){
    bool ok=false;events[0]=events[1]=0;
    if(ft_stopping || (write?(reg!=0x814e || length!=1):
        !((reg==0x8140 && length==6) || (reg==0x814e && (length==82 || length==1)) ||
          (reg==0x814f && length>=8 && length<=80 && !(length%8)))))return false;
    if(ft_read16(i2c,0x24) || (ft_read32(dma,0x08)&1)){
        loader_log_line("fire-touch: identity skipped, controller/DMA busy\n");return false;
    }
    uint64_t txpa=kvtophys((vm_offset_t)ft_tx),rxpa=kvtophys((vm_offset_t)ft_rx);
    if(txpa<0x50000000 || txpa>0x90000000-128 || rxpa<0x50000000 || rxpa>0x90000000-128 || txpa==rxpa){
        loader_log_line("fire-touch: identity DMA memory outside owned RAM\n");return false;
    }
    /* Separate cache-line-aligned buffers remain alive until DMA is reset. */
    memset(ft_tx,0,sizeof(ft_tx));memset(ft_rx,0xaa,sizeof(ft_rx));
    ft_tx[0]=reg>>8;ft_tx[1]=reg;ft_cache(true);
    const unsigned io_regs[]={4,8,0x10,0x14,0x18,0x1c,0x20,0x28,0x40,0x48,0x54,0x60};
    const unsigned dma_regs[]={4,0x18,0x1c,0x20,0x24,0x28};
    uint16_t saved_io[sizeof(io_regs)/sizeof(io_regs[0])];
    uint32_t saved_dma[sizeof(dma_regs)/sizeof(dma_regs[0])];
    for(unsigned i=0;i<sizeof(io_regs)/sizeof(io_regs[0]);i++)saved_io[i]=ft_read16(i2c,io_regs[i]);
    for(unsigned i=0;i<sizeof(dma_regs)/sizeof(dma_regs[0]);i++)saved_dma[i]=ft_read32(dma,dma_regs[i]);
    ft_write16(i2c,8,0);ft_write32(dma,4,0);
    ft_live_i2c=i2c;ft_live_dma=dma;
    ft_write32(dma,0x0c,2);ft_pause();ft_write32(dma,0x0c,0);
    /* Follow mt8173 auto-restart's separate write/read DMA messages. */
    for(unsigned phase=0;phase<(write?1u:2u);phase++){
        ft_write16(i2c,0x10,0x2c|(!write && phase==0?2:0));
        ft_write16(i2c,4,(0x5d<<1)|phase);
        ft_write16(i2c,0x0c,0x1f);ft_write16(i2c,0x38,1);
        unsigned count=phase==0?(write?length+2:2):length;
        ft_write16(i2c,0x14,count);ft_write16(i2c,0x18,write?1:2);
        ft_write32(dma,0,0);ft_write32(dma,0x18,phase);
        ft_write32(dma,phase==0?0x1c:0x20,(uint32_t)(phase==0?txpa:rxpa));
        ft_write32(dma,phase==0?0x24:0x28,count);
        ft_write32(dma,8,1);ft_write16(i2c,0x24,write?1:phase==0?0xc001:0x4001);
        events[phase]=ft_wait_event(i2c,!write && phase==0?0x11:1);
        if(!events[phase] || (events[phase]&6))break;
        if(write || phase==1)ok=true;
    }
    /* Stop before releasing/invalidating buffers, also on timeout or NACK. */
    ft_write16(i2c,8,0);ft_write16(i2c,0x50,1);
    ft_write32(dma,8,0);ft_write32(dma,0x0c,2);ft_pause();
    if(ft_read32(dma,8)&1){
        /* Keep reset asserted and original private addresses intact. Never
         * restore a different destination while the DMA engine says active. */
        ft_cache(false);
        loader_log_line("fire-touch: identity DMA did not stop; reset held, addresses retained\n");return false;
    }
    ft_write32(dma,0x0c,0);
    ft_write16(i2c,0x0c,0x1f);ft_write32(dma,0,0);ft_cache(false);
    for(unsigned i=0;i<sizeof(dma_regs)/sizeof(dma_regs[0]);i++)ft_write32(dma,dma_regs[i],saved_dma[i]);
    /* Restore interrupt masks last. */
    for(unsigned i=0;i<sizeof(io_regs)/sizeof(io_regs[0]);i++)if(io_regs[i]!=8)ft_write16(i2c,io_regs[i],saved_io[i]);
    ft_write16(i2c,8,saved_io[1]);
    ft_live_i2c=ft_live_dma=0;
    return ok;
}
static bool ft_identity(vm_offset_t i2c,vm_offset_t dma){
    char line[224];uint16_t events[2];
    bool ok=ft_transfer(i2c,dma,0x8140,6,false,events);
    bool numeric=true;
    for(unsigned i=0;i<4;i++)if(!((ft_rx[i]>='0' && ft_rx[i]<='9') || (i==3 && ft_rx[i]==0)))numeric=false;
    snprintf(line,sizeof(line),"fire-touch: identity ok=%u events=%#x/%#x bytes=%02x %02x %02x %02x %02x %02x txPA=%#llx rxPA=%#llx\n",
        ok&&numeric,events[0],events[1],ft_rx[0],ft_rx[1],ft_rx[2],ft_rx[3],ft_rx[4],ft_rx[5],
        kvtophys((vm_offset_t)ft_tx),kvtophys((vm_offset_t)ft_rx));loader_log_line(line);
    return ok&&numeric;
}
/* Thread context only: hakios_touch_post takes a mutex. Coordinates are the
 * panel's native800x1280, normalized for the existing donor HID bridge. */
static void ft_poll(vm_offset_t i2c,vm_offset_t dma,vm_offset_t wrap){
    unsigned seconds=220;PE_parse_boot_argn("fire_touch_seconds",&seconds,sizeof(seconds));
    unsigned short_read=0;PE_parse_boot_argn("fire_touch_short",&short_read,sizeof(short_read));
    short_read=short_read==1;
    if(seconds<1 || seconds>3600)seconds=220;
    uint64_t end=ft_ticks()+(uint64_t)seconds*13000000;
    unsigned active=0,x[10]={0},y[10]={0},failures=0,frames=0,contacts=0;
    uint64_t measure_sequence=0;
    int key=-1,candidate=-1;unsigned key_samples=0,key_events=0;
    loader_log_line("fire-touch: contact polling started800x1280; no firmware/config writes\n");
    while(!ft_stopping && ft_ticks()<end){
        uint16_t events[2];unsigned char frame[82];
        uint16_t status;
        /* Exact-board upmu_get_pwrkey_deb: CHRSTATUS142 bit1, zero=pressed.
         * Poll only; leave PMIC IRQ/reset/long-press configuration unchanged. */
        if(ft_pmic_read(wrap,0x142,&status)){
            int down=(status&2)==0;
            if(key<0)key=down;
            if(candidate!=down){candidate=down;key_samples=1;}
            else if(key_samples<2)key_samples++;
            if(key_samples==2 && key!=down){
                key=down;key_events++;
                hakios_touch_post(down?HAKIOS_BUTTON_DOWN:HAKIOS_BUTTON_UP,HAKIOS_BUTTON_LOCK,0,0);
            }
        }else key_samples=0;
        /* The Goodix ready frame remains latched until the existing ack.
         * Read status first; idle polls and zero-contact releases need no
         * coordinate payload. Read only the reported contacts when ready. */
        uint64_t measure_begin=fire_measure_clock(),measure_id=++measure_sequence;
        bool got=ft_transfer(i2c,dma,0x814e,short_read?1:sizeof(frame),false,events);
        uint64_t measure_status=fire_measure_clock(),measure_payload=measure_status,measure_ack=0;
        unsigned measure_flags=(got?256u:0u)|(short_read?1024u:0u);
        if(got){
            memcpy(frame,ft_rx,short_read?1:sizeof(frame));
            measure_flags|=frame[0];
            unsigned count=frame[0]&15;
            if(short_read && (frame[0]&0x80) && count && count<=10){
                got=ft_transfer(i2c,dma,0x814f,count*8,false,events);
                if(got)memcpy(frame+1,ft_rx,count*8);
            }
            measure_payload=fire_measure_clock();
        }
        if(!got){
            fire_measure_event(FNM_GOODIX,measure_begin,fire_measure_clock(),measure_id,measure_status,measure_payload,0,measure_flags);
            if(++failures>=5)break;
            delay_for_interval(20,NSEC_PER_MSEC);continue;
        }
        failures=0;
        if(frame[0]&0x80){
            unsigned count=frame[0]&15,seen=0,nx[10]={0},ny[10]={0};bool valid=count<=10;
            for(unsigned i=0;valid && i<count;i++){
                unsigned char *p=frame+1+8*i;unsigned id=p[0]&15;
                unsigned px=p[1]|(p[2]<<8),py=p[3]|(p[4]<<8);
                if((p[0]&0x80) || id>=10 || (seen&(1u<<id)) || px>=800 || py>=1280){valid=false;break;}
                seen|=1u<<id;nx[id]=px*65535/799;ny[id]=py*65535/1279;
            }
            /* Acknowledge the sampled frame, including malformed frames, exactly
             * as the donor Linux driver does. Never emit a partial frame. */
            if(!ft_transfer(i2c,dma,0x814e,1,true,events)){
                fire_measure_event(FNM_GOODIX,measure_begin,fire_measure_clock(),measure_id,measure_status,measure_payload,0,measure_flags);
                break;
            }
            measure_ack=fire_measure_clock();measure_flags|=2048u;
            if(valid){
                measure_flags|=512u;
                for(unsigned id=0;id<10;id++){
                    unsigned bit=1u<<id;
                    if(seen&bit){
                        if(!(active&bit) || nx[id]!=x[id] || ny[id]!=y[id])
                            hakios_touch_post(active&bit?HAKIOS_TOUCH_MOVE:HAKIOS_TOUCH_DOWN,id,nx[id],ny[id]);
                        if(!(active&bit))contacts++;
                        x[id]=nx[id];y[id]=ny[id];
                    }else if(active&bit)hakios_touch_post(HAKIOS_TOUCH_UP,id,x[id],y[id]);
                }
                active=seen;frames++;
            }
        }
        fire_measure_event(FNM_GOODIX,measure_begin,fire_measure_clock(),measure_id,measure_status,measure_payload,measure_ack,measure_flags);
        delay_for_interval(16,NSEC_PER_MSEC);
    }
    for(unsigned id=0;id<10;id++)if(active&(1u<<id))hakios_touch_post(HAKIOS_TOUCH_UP,id,x[id],y[id]);
    if(key==1)hakios_touch_post(HAKIOS_BUTTON_UP,HAKIOS_BUTTON_LOCK,0,0);
    char line[160];snprintf(line,sizeof(line),"fire-touch: polling ended frames=%u contacts=%u busfailures=%u keyevents=%u\n",frames,contacts,failures,key_events);loader_log_line(line);
}
void fire_touch_quiesce(void){
    /* Called with IRQs masked on the single Fire CPU. No locks, allocations,
     * scheduler calls or console output; static DMA buffers remain alive. */
    ft_stopping=true;
    if(ft_live_i2c)ft_write16(ft_live_i2c,8,0);
    if(ft_live_dma){ft_write32(ft_live_dma,8,0);ft_write32(ft_live_dma,0xc,2);}
}
/* Only touch regulator fields from the pinned MT6323 driver are writable.
 * Every update preserves peer fields and verifies the owned field. */
static int ft_pmic_update(vm_offset_t wrap,unsigned reg,unsigned mask,unsigned value){
    if(!((reg==0x50c || reg==0x536)?mask==0x8000:
         (reg==0x532 || reg==0x538)?mask==0xe0:0) || (value&~mask))return 0;
    uint16_t old,got;uint32_t word;
    if(!ft_pmic_read(wrap,reg,&old))return 0;
    if((old&mask)==value)return 1;
    if(!ft_wrap_wait(wrap,0,&word))return 0;
    ft_write32(wrap,0xa0,0x80000000|((reg>>1)<<16)|((old&~mask)|value));
    if(!ft_wrap_wait(wrap,0,&word) || !ft_pmic_read(wrap,reg,&got))return 0;
    return (got&mask)==value;
}
static unsigned ft_gpio_bit(vm_offset_t gpio,unsigned base,unsigned pin){return (ft_read32(gpio,base+(pin/16)*16)>>(pin%16))&1;}
static void ft_gpio_set(vm_offset_t gpio,unsigned base,unsigned pin,unsigned value){
    /* Atomic SET/CLEAR aliases; never rewrite another pin's state. */
    if((pin!=22 && pin!=45) || (base!=0 && base!=0x400))return;
    ft_write32(gpio,base+(pin/16)*16+(value?4:8),1u<<(pin%16));
}
static unsigned ft_gpio_mode(vm_offset_t gpio,unsigned pin){return (ft_read32(gpio,0x600+(pin/5)*16)>>(3*(pin%5)))&7;}
static void ft_power_identity(vm_offset_t i2c,vm_offset_t dma,bool poll){
    vm_offset_t wrap=ml_io_map(0x1000d000,0x1000),gpio=ml_io_map(0x10005000,0x1000);
    if(!wrap || !gpio){loader_log_line("fire-touch: powered probe mapping failed\n");return;}
    const unsigned regs[]={0x50c,0x532,0x536,0x538},masks[]={0x8000,0xe0,0x8000,0xe0};
    uint16_t cid,old[4];
    if(!ft_pmic_read(wrap,0x100,&cid) || cid!=0x2023){loader_log_line("fire-touch: powered probe unknown/unreadable PMIC\n");return;}
    for(unsigned i=0;i<4;i++)if(!ft_pmic_read(wrap,regs[i],&old[i])){loader_log_line("fire-touch: powered probe rail snapshot failed\n");return;}
    /* Do not take ownership of an already active rail or change pin muxes. */
    if((old[0]&0x8000) || (old[2]&0x8000) || ft_gpio_mode(gpio,22)!=0 || ft_gpio_mode(gpio,45)!=0 ||
        ft_gpio_mode(gpio,77)!=1 || ft_gpio_mode(gpio,78)!=1 || ft_read16(i2c,0x24) || (ft_read32(dma,8)&1)){
        loader_log_line("fire-touch: powered probe preconditions failed; unchanged\n");return;
    }
    unsigned dir22=ft_gpio_bit(gpio,0,22),out22=ft_gpio_bit(gpio,0x400,22);
    unsigned dir45=ft_gpio_bit(gpio,0,45),out45=ft_gpio_bit(gpio,0x400,45);
    ft_gpio_set(gpio,0x400,45,0);ft_gpio_set(gpio,0,45,1);
    ft_gpio_set(gpio,0x400,22,0);ft_gpio_set(gpio,0,22,1);
    int powered=0;
    if(ft_pmic_update(wrap,0x532,0xe0,0xc0) && ft_pmic_update(wrap,0x538,0xe0,0x60) &&
       ft_pmic_update(wrap,0x50c,0x8000,0x8000)){
        ft_delay(2);
        if(ft_pmic_update(wrap,0x536,0x8000,0x8000)){
            /* Goodix5d address: INT low throughout reset; allow both rails to settle. */
            ft_delay(20000);ft_gpio_set(gpio,0x400,45,1);ft_delay(6000);
            ft_delay(50000);ft_gpio_set(gpio,0,22,0);
            powered=1;if(ft_identity(i2c,dma) && poll)ft_poll(i2c,dma,wrap);
        }
    }
    /* All partial failures take the same rollback path. Reset before poweroff. */
    ft_gpio_set(gpio,0x400,45,0);ft_delay(1000);
    int restored=ft_pmic_update(wrap,0x536,0x8000,0);ft_delay(2);
    if(!ft_pmic_update(wrap,0x50c,0x8000,0))restored=0;
    for(unsigned i=0;i<4;i++)if(!ft_pmic_update(wrap,regs[i],masks[i],old[i]&masks[i]))restored=0;
    ft_gpio_set(gpio,0,22,0);ft_gpio_set(gpio,0,45,0);
    ft_gpio_set(gpio,0x400,22,out22);ft_gpio_set(gpio,0x400,45,out45);
    ft_gpio_set(gpio,0,22,dir22);ft_gpio_set(gpio,0,45,dir45);
    if(ft_gpio_bit(gpio,0,22)!=dir22 || ft_gpio_bit(gpio,0x400,22)!=out22 ||
       ft_gpio_bit(gpio,0,45)!=dir45 || ft_gpio_bit(gpio,0x400,45)!=out45)restored=0;
    char line[160];snprintf(line,sizeof(line),"fire-touch: powered identity attempted=%u rails/pins restored=%u\n",powered,restored);loader_log_line(line);
}
static void ft_probe_run(unsigned enabled){
    vm_offset_t clocks=ml_io_map(0x10001000,0x1000);
    vm_offset_t i2c=ml_io_map(0x11009000,0x1000);
    vm_offset_t dma_page=ml_io_map(0x11000000,0x1000);
    if(!clocks || !i2c || !dma_page){loader_log_line("fire-touch: register probe mapping failed\n");return;}
    vm_offset_t dma=dma_page+0x280;
    uint32_t old0=ft_read32(clocks,0x90),old1=ft_read32(clocks,0x94);
    /* Clear bits enable only I2C2/main and AP_DMA clocks. Never alter peers. */
    ft_write32(clocks,0x84,1u<<13);ft_write32(clocks,0x8c,1u<<18);
    char line[224];
    snprintf(line,sizeof(line),"fire-touch: clocks before %#x/%#x enabled %#x/%#x\n",
        old0,old1,ft_read32(clocks,0x90),ft_read32(clocks,0x94));loader_log_line(line);
    snprintf(line,sizeof(line),"fire-touch: i2c2 control=%#x timing=%#x hs=%#x start=%#x irq=%#x mask=%#x fifo=%#x io=%#x\n",
        ft_read16(i2c,0x10),ft_read16(i2c,0x20),ft_read16(i2c,0x48),ft_read16(i2c,0x24),
        ft_read16(i2c,0x0c),ft_read16(i2c,0x08),ft_read16(i2c,0x30),ft_read16(i2c,0x40));loader_log_line(line);
    snprintf(line,sizeof(line),"fire-touch: dma2 en=%#x con=%#x irq=%#x tx=%#x/%u rx=%#x/%u\n",
        ft_read32(dma,0x08),ft_read32(dma,0x18),ft_read32(dma,0x00),
        ft_read32(dma,0x1c),ft_read32(dma,0x24),ft_read32(dma,0x20),ft_read32(dma,0x28));loader_log_line(line);
    if(enabled==2)ft_identity(i2c,dma);
    if(enabled==3)ft_power_snapshot();
    if(enabled==4 || enabled==5)ft_power_identity(i2c,dma,enabled==5);
    /* Any stuck engine was already left in reset by transfer cleanup. */
    ft_live_i2c=ft_live_dma=0;
    ft_write32(clocks,0x80,old0&(1u<<13));ft_write32(clocks,0x88,old1&(1u<<18));
    uint32_t now0=ft_read32(clocks,0x90),now1=ft_read32(clocks,0x94);
    snprintf(line,sizeof(line),"fire-touch: gates restored=%u status=%#x/%#x; probe complete\n",
        ((now0^old0)&(1u<<13))==0 && ((now1^old1)&(1u<<18))==0,now0,now1);loader_log_line(line);
}
static void ft_thread(void *arg,wait_result_t result){
    (void)arg;(void)result;ft_probe_run(5);thread_terminate(current_thread());
}
void fire_touch_probe(void){
    uint32_t enabled=0;
    if(fire_touch_probed || !PE_parse_boot_argn("fire_touch_probe",&enabled,sizeof(enabled)) || enabled<1 || enabled>5)return;
    fire_touch_probed=true;
    if(enabled==5){
        thread_t thread;
        if(kernel_thread_start(ft_thread,NULL,&thread)==KERN_SUCCESS)thread_deallocate(thread);
        else loader_log_line("fire-touch: cannot start polling thread\n");
    }else ft_probe_run(enabled);
}
#endif
