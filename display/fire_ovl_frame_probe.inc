/* Diagnostic only. Exact live DT OVL0: SPI184, level-low, GIC216.
 * Vendor ddp_irq.c defines INTSTA bit1=frame complete and complement ack.
 * Default has no address, geometry, routing, clock or framebuffer writes.
 * Explicit diagnostic-only pixel mode changes/restores one memory pixel. These events
 * establish timing prerequisites, not proof of address latch or retirement. */
extern int fire_ovl_gic_attach(uint32_t *,void(*)(void *,uint32_t),void *);
extern int fire_ovl_gic_restore(uint32_t *);
extern unsigned fire_ovl_gic_burst(void);
static vm_offset_t fop_bank;
static uint32_t fop_saved[8],fop_inten;
static unsigned fop_attempted,fop_armed,fop_attached,fop_restored,fop_reason;
static unsigned fop_skip_transparent,fop_alpha_verified,fop_source_restored,fop_inten_restored;
static uint32_t fop_source_saved;
static uint32_t fop_rdma3_saved;
static unsigned fop_rdma3_restored;
static unsigned fop_skip_requested;
static unsigned fop_apply_allowed;
static uint64_t fop_apply_at;
static uint32_t fop_apply_flow,fop_apply_source,fop_apply_rdma3;
static uint64_t fop_calls,fop_complete,fop_abnormal,fop_spurious,fop_first,fop_last;
static uint64_t fop_bits[32];
static uint32_t fop_status_or;
struct fop_sample {uint64_t at,rdma_calls;uint32_t status,flow,rdma_status,address,rdma_debug,position;};
static struct fop_sample fop_samples[32];
static struct fop_sample fop_faults[32];
static unsigned fop_count,fop_fault_count;
#if defined(FIRE_OVL_HANDOFF_PROBE)
#include "fire_ovl_handoff_probe.inc"
#endif
#if defined(FIRE_OVL_PIXEL_PROBE)
/* Isolated one-pixel memory witness, never enabled in a UI/performance kernel.
 * It validates whether raw output data can observe a known source value;
 * it does not establish a whole-frame checksum, latch or retirement. */
#define FOP_PIXEL_OFFSET (800u*1280u*4u-4u)
static vm_offset_t fop_pixel_base;
static unsigned fop_pixel_ready,fop_pixel_stage,fop_pixel_count,fop_pixel_reason;
static unsigned fop_pixel_writes,fop_pixel_restored;
static uint32_t fop_pixel_saved,fop_pixel_expected;
struct fop_pixel_sample {uint64_t at;uint32_t phase,source,debug,position,status,flow;};
static struct fop_pixel_sample fop_pixel_samples[48];
static void fop_pixel_init(void)
{
    vm_offset_t larb=ml_io_map(0x14016000,4096);
    if(!larb || (fu_r32(larb,0xf00)&1u) || fu_r32(fop_bank,0x30)!=0x010021ff ||
       fu_r32(fop_bank,0x38)!=0x05000320 || fu_r32(fop_bank,0x3c)!=0 ||
       (fu_r32(fop_bank,0x44)&0xffff)!=3200){fop_pixel_reason=1;return;}
    fop_pixel_base=ml_io_map(0x98800000,800u*1280u*4u);
    if(!fop_pixel_base){fop_pixel_reason=2;return;}
    fop_pixel_ready=1;
}
static void fop_pixel_write(uint32_t value)
{
    /* Existing store helper supplies DSB SY. The only RAM offset allowed here
     * is the last pixel of the exact inherited800x1280/3200-byte layout. */
    fu_scanout_write(fop_pixel_base,FOP_PIXEL_OFFSET,value);
    fop_pixel_expected=value;fop_pixel_writes++;
}
static void fop_pixel_irq(uint64_t at,unsigned status)
{
    if(!fop_pixel_ready || fop_pixel_reason || fop_pixel_stage>=4 ||
       !__atomic_load_n(&fop_apply_allowed,__ATOMIC_ACQUIRE) || fop_count<32 || !(status&2))return;
    uint32_t flow=fu_r32(fop_bank,0x240);
    if((flow&0x3ff)!=2 || (flow&0xf0000)!=0xf0000)return;
    if(!fop_pixel_stage){
        fop_pixel_saved=fu_r32(fop_pixel_base,FOP_PIXEL_OFFSET);
        fop_pixel_stage=1;fop_pixel_write(0xff13579b);return;
    }
    uint32_t source=fu_r32(fop_pixel_base,FOP_PIXEL_OFFSET);
    /* Another writer invalidates the experiment. Do not overwrite its newer
     * pixel with our old snapshot in shutdown. No UI client is admitted. */
    if(source!=fop_pixel_expected){fop_pixel_reason=3;return;}
    if(fop_pixel_count>=48){fop_pixel_reason=4;return;}
    struct fop_pixel_sample *s=&fop_pixel_samples[fop_pixel_count++];
    s->at=at;s->phase=fop_pixel_stage;s->source=source;s->status=status;s->flow=flow;
    s->debug=fu_r32(fop_bank,0x24c);s->position=fu_r32(fop_bank,0x244);
    if(fop_pixel_count==16){fop_pixel_stage=2;fop_pixel_write(0xffe2a46d);}
    else if(fop_pixel_count==32){fop_pixel_stage=3;fop_pixel_write(fop_pixel_saved);
        fop_pixel_restored=fu_r32(fop_pixel_base,FOP_PIXEL_OFFSET)==fop_pixel_saved;}
    else if(fop_pixel_count==48)fop_pixel_stage=4;
}
static void fop_pixel_stop(void)
{
    if(!fop_pixel_writes || fop_pixel_stage>=3 || fop_pixel_reason)return;
    struct fire_display_io io={NULL,fso_read};struct fire_display_snapshot gate={0};
    if(fire_display_module_gate(&io,&gate,1u<<10)){fop_pixel_reason=5;return;}
    if(fu_r32(fop_pixel_base,FOP_PIXEL_OFFSET)!=fop_pixel_expected){fop_pixel_reason=3;return;}
    fop_pixel_write(fop_pixel_saved);
    fop_pixel_restored=fu_r32(fop_pixel_base,FOP_PIXEL_OFFSET)==fop_pixel_saved;
    fop_pixel_stage=5; /* interrupted witness: restored, but never complete */
}
static void fop_pixel_report(void)
{
    char line[256];snprintf(line,sizeof(line),"fire-ovl-pixel: ready=%u stage=%u count=%u reason=%u writes=%u restored=%u saved=%#x\n",fop_pixel_ready,fop_pixel_stage,fop_pixel_count,fop_pixel_reason,fop_pixel_writes,fop_pixel_restored,fop_pixel_saved);loader_log_line(line);
    for(unsigned i=0;i<fop_pixel_count;i++){struct fop_pixel_sample *s=&fop_pixel_samples[i];
        snprintf(line,sizeof(line),"fire-ovl-pixel-sample: index=%u at=%llu phase=%u source=%#x debug=%#x position=%#x status=%#x flow=%#x\n",i,s->at,s->phase,s->source,s->debug,s->position,s->status,s->flow);loader_log_line(line);}
}
#endif
static void fop_gamma_snapshot(void)
{
    /* Exact live DT disp_gamma@1400f000; vendor MM0 clock bit18.
     * ddp_reg.h defines CFG checksum enable/select and CHKSUM at0x2c.
     * Observe only: a value with checksum disabled is not frame evidence. */
    struct fire_display_io io={NULL,fso_read};struct fire_display_snapshot gate={0};
    unsigned code=fire_display_module_gate(&io,&gate,1u<<18);
    char line[256];
    if(code){snprintf(line,sizeof(line),"fire-ovl-gamma: gate=%u\n",code);loader_log_line(line);return;}
    vm_offset_t gamma=ml_io_map(0x1400f000,4096);
    if(!gamma){loader_log_line("fire-ovl-gamma: gate=8\n");return;}
    snprintf(line,sizeof(line),"fire-ovl-gamma: gate=0 en=%#x cfg=%#x size=%#x checksum=%#x status=%#x in=%#x out=%#x\n",fu_r32(gamma,0),fu_r32(gamma,0x20),fu_r32(gamma,0x30),fu_r32(gamma,0x2c),fu_r32(gamma,0x10),fu_r32(gamma,0x24),fu_r32(gamma,0x28));loader_log_line(line);
}
static void fop_dither_snapshot(void)
{
    /* Pinned live DT: DITHER14010000; MT8163 clock table MM0 bit19.
     * Vendor DITHER_REG(base,index)=base+0x100+index*4. Read only; do not
     * enable CRC, alter pixel processing, routing or clocks in this probe. */
    struct fire_display_io io={NULL,fso_read};struct fire_display_snapshot gate={0};
    unsigned code=fire_display_module_gate(&io,&gate,1u<<19);
    char line[256];
    if(code){snprintf(line,sizeof(line),"fire-ovl-dither: gate=%u\n",code);loader_log_line(line);return;}
    vm_offset_t dither=ml_io_map(0x14010000,4096);
    if(!dither){loader_log_line("fire-ovl-dither: gate=8\n");return;}
    snprintf(line,sizeof(line),"fire-ovl-dither: gate=0 en=%#x cfg=%#x size=%#x control=%#x result=%#x in=%#x out=%#x\n",fu_r32(dither,0),fu_r32(dither,0x20),fu_r32(dither,0x30),fu_r32(dither,0x100),fu_r32(dither,0x144),fu_r32(dither,0x24),fu_r32(dither,0x28));loader_log_line(line);
}
static void fop_irq(void *ctx,uint32_t id)
{
    (void)ctx;
    if(id!=216 || !__atomic_load_n(&fop_armed,__ATOMIC_ACQUIRE)){gic_v3_spi_mask(216);return;}
    if(fire_ovl_gic_burst()>64){fop_reason=1;gic_v3_spi_mask(216);return;}
    struct fire_display_io io={NULL,fso_read};struct fire_display_snapshot gate={0};
    if(fire_display_module_gate(&io,&gate,1u<<10)){fop_reason=2;gic_v3_spi_mask(216);return;}
    uint64_t at=mach_absolute_time();unsigned status=fu_r32(fop_bank,8);
    fop_calls++;if(!status)fop_spurious++;
#if defined(FIRE_OVL_SKIP_TRANSPARENT)
    if(__atomic_load_n(&fop_apply_allowed,__ATOMIC_ACQUIRE) && fop_skip_requested && (status&2) && !(status&~3u)){
        uint32_t flow=fu_r32(fop_bank,0x240);
        unsigned state=flow&0x3ff;
        /* Vendor idle/wait_SOF states, plus all four OVL read engines idle.
         * Apply at the observed completed-frame boundary, never mid-frame. */
        if((state==1 || state==2) && (flow&0xf0000)==0xf0000){
            fop_skip_requested=0;fop_skip_transparent=1;
            fop_apply_at=at;fop_apply_flow=flow;
            fu_scanout_write(fop_bank,0x120,0);
            fu_scanout_write(fop_bank,0x2c,1);
            fop_apply_source=fu_r32(fop_bank,0x2c);
            fop_apply_rdma3=fu_r32(fop_bank,0x120)&1u;
            if(fop_apply_source!=1 || fop_apply_rdma3)fop_reason=12;
        }
    }
#endif
    /* Retain the inherited first delivery but exclude it from interval data. */
    if(fop_calls>1 && (status&2)){
        fop_complete++;if(!fop_first)fop_first=at;fop_last=at;
        if(fop_count<32){struct fop_sample *s=&fop_samples[fop_count++];
            s->at=at;s->rdma_calls=FSI_LOAD(&fso.calls);s->status=status;
            s->flow=fu_r32(fop_bank,0x240);s->rdma_status=fu_r32(fso_banks[4],4);
            s->address=fu_r32(fop_bank,0xf40);
            s->rdma_debug=fu_r32(fop_bank,0x24c);s->position=fu_r32(fop_bank,0x244);}
        fop_status_or|=status;
        for(unsigned i=0;i<32;i++)fop_bits[i]+=(status>>i)&1u;
        if(status&~3u){
            fop_abnormal++;
            if(fop_fault_count<32){struct fop_sample *s=&fop_faults[fop_fault_count++];
                s->at=at;s->rdma_calls=FSI_LOAD(&fso.calls);s->status=status;
                s->flow=fu_r32(fop_bank,0x240);s->rdma_status=fu_r32(fso_banks[4],4);
                s->address=fu_r32(fop_bank,0xf40);
                s->rdma_debug=fu_r32(fop_bank,0x24c);s->position=fu_r32(fop_bank,0x244);}
        }
    }
#if defined(FIRE_OVL_PIXEL_PROBE)
    fop_pixel_irq(at,status);
#endif
#if defined(FIRE_OVL_HANDOFF_PROBE)
    foh_irq(at,status);
#endif
    if(status)fu_scanout_write(fop_bank,8,~status);
}
static void fop_start(void)
{
    if(!fso_attached || fso_gate || !FSI_LOAD(&fso.armed))return;
    fop_attempted=1;struct fire_display_io io={NULL,fso_read};struct fire_display_snapshot gate={0};
    if(fire_display_module_gate(&io,&gate,1u<<10)){fop_reason=3;return;}
    fop_bank=ml_io_map(FDP_OVL,4096);if(!fop_bank){fop_reason=4;return;}
    if(fu_r32(fop_bank,0xc)!=1 || fu_r32(fop_bank,0x20)!=0x05000320 ||
       fu_r32(fop_bank,0x2c)!=9 || fu_r32(fop_bank,0xf40)!=0x98800000){fop_reason=5;return;}
    fop_source_saved=fu_r32(fop_bank,0x2c);
    fop_rdma3_saved=fu_r32(fop_bank,0x120)&1u;
    fop_dither_snapshot();
    fop_gamma_snapshot();
#if defined(FIRE_OVL_HANDOFF_PROBE)
    foh_init();
#endif
#if defined(FIRE_OVL_PIXEL_PROBE)
    fop_pixel_init();
#endif
#if defined(FIRE_OVL_SKIP_TRANSPARENT)
    /* Exact inherited physical OVL mappings only. Verify format/alpha mode,
     * top-layer layout and every alpha byte before omitting its memory fetch.
     * No other writer owns this immutable LK layer in the diagnostic root. */
    vm_offset_t larb=ml_io_map(0x14016000,4096);
    if(!larb || (fu_r32(larb,0xf00)&1u) || fu_r32(fop_bank,0x90)!=0x010021ff ||
       fu_r32(fop_bank,0x98)!=0x05000320 || fu_r32(fop_bank,0x9c)!=0 ||
       (fu_r32(fop_bank,0xa4)&0xffff)!=3200 || fu_r32(fop_bank,0xfa0)!=0x98be8000 ||
       !(fu_r32(fop_bank,0x120)&1u)){fop_reason=9;return;}
    volatile uint32_t *top=(volatile uint32_t *)ml_io_map(0x98be8000,800*1280*4);
    if(!top){fop_reason=10;return;}
    for(unsigned i=0;i<800*1280;i++)if(top[i]>>24){fop_reason=11;return;}
    fop_alpha_verified=1;
    fop_skip_requested=1;
#endif
    fop_inten=fu_r32(fop_bank,4);
    __atomic_store_n(&fop_armed,1u,__ATOMIC_RELEASE);
    int code=fire_ovl_gic_attach(fop_saved,fop_irq,NULL);
    fop_attached=code==KERN_SUCCESS || fop_saved[0]==216;
    if(code!=KERN_SUCCESS){__atomic_store_n(&fop_armed,0u,__ATOMIC_RELEASE);fop_reason=6;return;}
    /* Attach unmasks before its final route checks. An inherited IRQ may run
     * inside attach; it must not mutate layer state until ownership succeeds. */
    __atomic_store_n(&fop_apply_allowed,1u,__ATOMIC_RELEASE);
    fu_scanout_write(fop_bank,4,2); /* frame complete only; saved mask restored */
    char line[256];snprintf(line,sizeof(line),"fire-ovl-probe: start code=%d inten_saved=%#x polarity_saved=%u polarity_applied=%u skip_transparent=%u alpha_verified=%u source_saved=%u source_live=%u\n",code,fop_inten,fop_saved[6],fop_saved[7],fop_alpha_verified,fop_alpha_verified,fop_source_saved,fu_r32(fop_bank,0x2c));loader_log_line(line);
}
static void fop_stop(void)
{
    if(!fop_attached || fop_restored)return;
    boolean_t irq=ml_set_interrupts_enabled(FALSE);
    gic_v3_spi_mask(216);__atomic_store_n(&fop_armed,0u,__ATOMIC_RELEASE);
    __atomic_store_n(&fop_apply_allowed,0u,__ATOMIC_RELEASE);
#if defined(FIRE_OVL_PIXEL_PROBE)
    fop_pixel_stop();
#endif
#if defined(FIRE_OVL_HANDOFF_PROBE)
    foh_stop();
#endif
    /* No new register accesses after power loss; failed restore stays explicit. */
    struct fire_display_io io={NULL,fso_read};struct fire_display_snapshot gate={0};
    if(!fire_display_module_gate(&io,&gate,1u<<10)){
        if(fop_skip_transparent){
            fu_scanout_write(fop_bank,0x120,fop_rdma3_saved);
            fu_scanout_write(fop_bank,0x2c,fop_source_saved);
        }
        fop_rdma3_restored=(fu_r32(fop_bank,0x120)&1u)==fop_rdma3_saved;
        fop_source_restored=fu_r32(fop_bank,0x2c)==fop_source_saved;
        fu_scanout_write(fop_bank,4,fop_inten);
        fop_inten_restored=fu_r32(fop_bank,4)==fop_inten;
        if(!fop_source_restored || !fop_inten_restored || !fop_rdma3_restored)fop_reason=8;
    }
    else fop_reason=7;
    fop_restored=fire_ovl_gic_restore(fop_saved)!=0;
    ml_set_interrupts_enabled(irq);
}
static void fop_report(void)
{
    if(!fop_attempted)return;
#if defined(FIRE_OVL_HANDOFF_PROBE)
    foh_report();
#endif
#if defined(FIRE_OVL_PIXEL_PROBE)
    fop_pixel_report();
#endif
    char line[320];snprintf(line,sizeof(line),"fire-ovl-probe: stop attached=%u restored=%u reason=%u calls=%llu complete=%llu abnormal=%llu spurious=%llu first=%llu last=%llu samples=%u\n",fop_attached,fop_restored,fop_reason,fop_calls,fop_complete,fop_abnormal,fop_spurious,fop_first,fop_last,fop_count);loader_log_line(line);
    for(unsigned i=0;i<fop_count;i++){struct fop_sample *s=&fop_samples[i];
        snprintf(line,sizeof(line),"fire-ovl-sample: index=%u at=%llu rdma_calls=%llu status=%#x flow=%#x rdma_status=%#x address=%#x rdma_debug=%#x position=%#x\n",i,s->at,s->rdma_calls,s->status,s->flow,s->rdma_status,s->address,s->rdma_debug,s->position);loader_log_line(line);}
    snprintf(line,sizeof(line),"fire-ovl-restore: source=%u inten=%u rdma3=%u source_live=%u inten_saved=%u rdma3_saved=%u\n",fop_source_restored,fop_inten_restored,fop_rdma3_restored,fop_source_saved,fop_inten,fop_rdma3_saved);loader_log_line(line);
    snprintf(line,sizeof(line),"fire-ovl-plane: applied=%u pending=%u at=%llu flow=%#x source=%u rdma3=%u\n",fop_skip_transparent,fop_skip_requested,fop_apply_at,fop_apply_flow,fop_apply_source,fop_apply_rdma3);loader_log_line(line);
    snprintf(line,sizeof(line),"fire-ovl-status: status_or=%#x faults_saved=%u\n",fop_status_or,fop_fault_count);loader_log_line(line);
    for(unsigned i=0;i<32;i++)if(fop_bits[i]){
        snprintf(line,sizeof(line),"fire-ovl-bit: bit=%u count=%llu\n",i,fop_bits[i]);loader_log_line(line);}
    for(unsigned i=0;i<fop_fault_count;i++){struct fop_sample *s=&fop_faults[i];
        snprintf(line,sizeof(line),"fire-ovl-fault: index=%u at=%llu rdma_calls=%llu status=%#x flow=%#x rdma_status=%#x address=%#x rdma_debug=%#x position=%#x\n",i,s->at,s->rdma_calls,s->status,s->flow,s->rdma_status,s->address,s->rdma_debug,s->position);loader_log_line(line);}
}
static unsigned fop_live_stats(char *out,unsigned cap)
{
    if(!fop_attempted || cap<512)return 0;
    /* USB owner and OVL IRQ are both bound to the boot CPU. Snapshot memory
     * only; no live register reads, allocations or printing with IRQs masked. */
    boolean_t irq=ml_set_interrupts_enabled(FALSE);
    uint64_t at=mach_absolute_time(),calls=fop_calls,complete=fop_complete,abnormal=fop_abnormal,bits[32];
    unsigned reason=fop_reason,armed=fop_armed,skip=fop_skip_transparent,pending=fop_skip_requested;
    uint32_t status=fop_status_or,flow=fop_apply_flow,source=fop_apply_source,rdma=fop_apply_rdma3;
    uint64_t applied=fop_apply_at;memcpy(bits,fop_bits,sizeof(bits));
#if defined(FIRE_OVL_HANDOFF_PROBE)
    unsigned hr=foh_ready,hp=foh_phase,hc=foh_count,he=foh_reason,hw=foh_writes,hs=foh_restored,hx=foh_emergency,hd=foh_done;
    uint64_t hb=foh_retained;
#if defined(FIRE_OVL_REUSE_PROBE)
    unsigned hu=foh_rewritten && foh_rewrite_cleaned;
#endif
#endif
    ml_set_interrupts_enabled(irq);
    int n=snprintf(out,cap,"# hw_ovl_probe version=1 at=%llu calls=%llu complete=%llu abnormal=%llu reason=%u armed=%u status_or=%#x skip_applied=%u skip_pending=%u apply_at=%llu apply_flow=%#x apply_source=%u apply_rdma3=%u\n",(unsigned long long)at,(unsigned long long)calls,(unsigned long long)complete,(unsigned long long)abnormal,reason,armed,status,skip,pending,(unsigned long long)applied,flow,source,rdma);
    if(n<0 || (unsigned)n>=cap)return 0;
    unsigned used=(unsigned)n;
    for(unsigned i=0;i<32;i++)if(bits[i]){
        n=snprintf(out+used,cap-used,"# hw_ovl_bit bit=%u count=%llu\n",i,(unsigned long long)bits[i]);
        if(n<0 || (unsigned)n>=cap-used)return used;used+=(unsigned)n;
    }
#if defined(FIRE_OVL_HANDOFF_PROBE)
#if defined(FIRE_OVL_REUSE_PROBE)
    n=snprintf(out+used,cap-used,"# hw_ovl_handoff version=2 ready=%u phase=%u count=%u reason=%u writes=%u restored=%u emergency=%u done=%u retained=%llu rewritten=%u\n",hr,hp,hc,he,hw,hs,hx,hd,(unsigned long long)hb,hu);
#else
    n=snprintf(out+used,cap-used,"# hw_ovl_handoff version=1 ready=%u phase=%u count=%u reason=%u writes=%u restored=%u emergency=%u done=%u retained=%llu\n",hr,hp,hc,he,hw,hs,hx,hd,(unsigned long long)hb);
#endif
    if(n<0 || (unsigned)n>=cap-used)return used;used+=(unsigned)n;
#endif
    return used;
}
