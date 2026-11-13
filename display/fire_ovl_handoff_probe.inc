/* Isolated one-core/panel diagnostic. All private allocations remain held.
 * Explicit reuse mode rewrites one retired private marker after B is consumed.
 * Only L0_ADDR changes; source mask, RDMA controls, format, clocks and pixels
 * in the original framebuffer remain untouched. IRQ records are bounded. */
#include <mach/vm_statistics.h>
#include <kern/sched_prim.h>
extern uint64_t kvtophys(vm_offset_t);
#define FOH_BYTES (800u*1280u*4u)
#define FOH_ORIGINAL 0x98800000u
#if defined(FIRE_OVL_REUSE_PROBE)
#define FOH_ORIGINAL_PHASE 5u
#define FOH_DONE_PHASE 6u
#define FOH_MAX_SAMPLES 160u
static unsigned foh_rewritten,foh_rewrite_cleaned;
static uint64_t foh_rewrite_at;
static uint32_t foh_rewrite_status,foh_rewrite_flow;
#else
#define FOH_ORIGINAL_PHASE 3u
#define FOH_DONE_PHASE 4u
#define FOH_MAX_SAMPLES 96u
#endif
static vm_offset_t foh_original,foh_buffers[2];
static uint32_t foh_pa[2],foh_markers[2]={0xff13579b,0xffe2a46d};
static unsigned foh_attempted,foh_ready,foh_phase,foh_count,foh_phase_seen,foh_matches;
static unsigned foh_reason,foh_writes,foh_restored,foh_emergency,foh_stop_requested,foh_done;
static uint64_t foh_retained,foh_request_at;
struct foh_sample {uint64_t at,request_at;uint32_t phase,address,expected,source,debug,position,status,flow,match;};
static struct foh_sample foh_samples[FOH_MAX_SAMPLES];
struct foh_write {uint64_t at;uint32_t address,status,flow;};
static struct foh_write foh_write_log[4];
static uint32_t foh_rgb(uint32_t value){return (value&255)<<16 | (value&0xff00) | (value>>16&255);}
static void foh_init(void)
{
    if(foh_attempted)return;foh_attempted=1;
    vm_offset_t larb=ml_io_map(0x14016000,4096);
    if(!larb || (fu_r32(larb,0xf00)&1u) || fu_r32(fop_bank,0x30)!=0x010021ff ||
       fu_r32(fop_bank,0x38)!=0x05000320 || fu_r32(fop_bank,0x3c)!=0 ||
       (fu_r32(fop_bank,0x44)&0xffff)!=3200){foh_reason=1;return;}
    foh_original=ml_io_map(FOH_ORIGINAL,FOH_BYTES);
    if(!foh_original){foh_reason=2;return;}
    for(unsigned i=0;i<2;i++){
        if(kmem_alloc_contig(kernel_map,&foh_buffers[i],FOH_BYTES,PAGE_MASK,
            (ppnum_t)(0xffffffffull>>PAGE_SHIFT),0,KMA_KOBJECT|KMA_LOMEM,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS){foh_reason=3;return;}
        foh_retained+=FOH_BYTES;
        uint64_t pa=kvtophys(foh_buffers[i]);
        if(!pa || (pa&PAGE_MASK) || pa+FOH_BYTES>0x100000000ull ||
           (pa<0x98fd0000ull && pa+FOH_BYTES>FOH_ORIGINAL)){foh_reason=4;return;}
        for(unsigned offset=0;offset<FOH_BYTES;offset+=PAGE_SIZE)
            if(kvtophys(foh_buffers[i]+offset)!=pa+offset){foh_reason=4;return;}
        if(kvtophys(foh_buffers[i]+FOH_BYTES-1)!=pa+FOH_BYTES-1){foh_reason=4;return;}
        if(i && pa<foh_pa[0]+(uint64_t)FOH_BYTES && pa+FOH_BYTES>foh_pa[0]){foh_reason=4;return;}
        foh_pa[i]=(uint32_t)pa;
        uint32_t *to=(uint32_t *)foh_buffers[i];volatile uint32_t *from=(volatile uint32_t *)foh_original;
        for(unsigned j=0;j<FOH_BYTES/4;j++)to[j]=from[j];
        to[FOH_BYTES/4-1]=foh_markers[i];
        CleanPoC_DcacheRegion(foh_buffers[i],FOH_BYTES);
    }
    __atomic_store_n(&foh_ready,1u,__ATOMIC_RELEASE);
}
static void foh_program(uint32_t address,uint64_t at,unsigned status,uint32_t flow)
{
    if(foh_writes<4)foh_write_log[foh_writes]=(struct foh_write){at,address,status,flow};
    foh_writes++;foh_request_at=at;
    fu_scanout_write(fop_bank,0xf40,address); /* existing DSB SY store */
    if(fu_r32(fop_bank,0xf40)!=address)foh_reason=5;
    foh_phase_seen=foh_matches=0;
}
static void foh_finish(void)
{
    __atomic_store_n(&foh_done,1u,__ATOMIC_RELEASE);
    thread_wakeup((event_t)&foh_done);
}
static void foh_irq(uint64_t at,unsigned status)
{
    if(!__atomic_load_n(&foh_ready,__ATOMIC_ACQUIRE) || foh_phase>=FOH_DONE_PHASE ||
       !__atomic_load_n(&fop_apply_allowed,__ATOMIC_ACQUIRE) || fop_count<32 || !(status&2))return;
    uint32_t flow=fu_r32(fop_bank,0x240);
    if((flow&0x3ff)!=2 || (flow&0xf0000)!=0xf0000)return;
    if(!foh_phase){
        if(foh_stop_requested || foh_reason)return;
        if(fu_r32(fop_bank,0xf40)!=FOH_ORIGINAL){foh_reason=6;return;}
        uint32_t rgb=foh_rgb(fu_r32(foh_original,FOH_BYTES-4));
        if(rgb==foh_rgb(foh_markers[0]) || rgb==foh_rgb(foh_markers[1])){foh_reason=6;return;}
        foh_phase=1;foh_program(foh_pa[0],at,status,flow);return;
    }
    /* A failure or normal shutdown request stops new private handoffs. It
     * restores at this real idle boundary while all allocations remain held. */
    if((foh_stop_requested || foh_reason) && foh_phase<FOH_ORIGINAL_PHASE){
        foh_phase=FOH_ORIGINAL_PHASE;foh_program(FOH_ORIGINAL,at,status,flow);return;
    }
    if(foh_count>=FOH_MAX_SAMPLES){foh_reason=7;foh_phase=FOH_DONE_PHASE+1;foh_finish();return;}
    unsigned slot=foh_phase-1;
#if defined(FIRE_OVL_REUSE_PROBE)
    if(foh_phase==3)slot=1;
    if(foh_phase==4)slot=0;
#endif
    uint32_t expected=foh_phase==FOH_ORIGINAL_PHASE?FOH_ORIGINAL:foh_pa[slot];
    uint32_t source=foh_phase==FOH_ORIGINAL_PHASE?fu_r32(foh_original,FOH_BYTES-4):foh_markers[slot];
    struct foh_sample *s=&foh_samples[foh_count++];
    *s=(struct foh_sample){at,foh_request_at,foh_phase,fu_r32(fop_bank,0xf40),expected,source,
        fu_r32(fop_bank,0x24c),fu_r32(fop_bank,0x244),status,flow,0};
    s->match=s->address==expected && (s->debug>>4&0xffffff)==foh_rgb(source);
    if(foh_phase==FOH_ORIGINAL_PHASE && (foh_rgb(source)==foh_rgb(foh_markers[0]) ||
        foh_rgb(source)==foh_rgb(foh_markers[1])))s->match=0;
#if defined(FIRE_OVL_REUSE_PROBE)
    if(foh_phase==FOH_ORIGINAL_PHASE && foh_rgb(source)==foh_rgb(0xff13579b))s->match=0;
#endif
    foh_phase_seen++;foh_matches=s->match?foh_matches+1:0;
    if(foh_matches==16){
        if(foh_phase==1){foh_phase=2;foh_program(foh_pa[1],at,status,flow);}
#if defined(FIRE_OVL_REUSE_PROBE)
        else if(foh_phase==2){
            /* B has16 actual consumed-frame markers and all readers are idle.
             * A is no longer front/pending. Keep B selected while rewriting A;
             * no allocation, logging, wait or original-framebuffer write. */
            uint32_t *pixel=(uint32_t *)(foh_buffers[0]+FOH_BYTES-4);
            if(foh_rewritten || fu_r32(fop_bank,0xf40)!=foh_pa[1] || *pixel!=0xff13579b){foh_reason=12;return;}
            foh_rewrite_at=at;foh_rewrite_status=status;foh_rewrite_flow=flow;
            *pixel=0xff71c35e;
            CleanPoC_DcacheRegion((vm_offset_t)pixel,4);
            foh_rewrite_cleaned=1;foh_rewritten=1;foh_markers[0]=0xff71c35e;
            foh_phase=3;foh_request_at=at;foh_phase_seen=foh_matches=0;
        }
        else if(foh_phase==3){foh_phase=4;foh_program(foh_pa[0],at,status,flow);}
        else if(foh_phase==4){foh_phase=FOH_ORIGINAL_PHASE;foh_program(FOH_ORIGINAL,at,status,flow);}
#else
        else if(foh_phase==2){foh_phase=FOH_ORIGINAL_PHASE;foh_program(FOH_ORIGINAL,at,status,flow);}
#endif
        else {foh_restored=1;foh_phase=FOH_DONE_PHASE;foh_finish();}
    }else if(foh_phase_seen>=32){
        foh_reason=8;
        if(foh_phase<FOH_ORIGINAL_PHASE){foh_phase=FOH_ORIGINAL_PHASE;foh_program(FOH_ORIGINAL,at,status,flow);}
        else {foh_phase=FOH_DONE_PHASE+1;foh_finish();}
    }
}
/* Thread-context normal-return drain. IRQ wake or a bounded deadline only;
 * no polling, allocations, logging or waiting in the masked return hook. */
static void foh_drain(void)
{
    if(!foh_writes || __atomic_load_n(&foh_done,__ATOMIC_ACQUIRE))return;
    if(!ml_get_interrupts_enabled()){foh_reason=9;return;}
    boolean_t irq=ml_set_interrupts_enabled(FALSE);
    foh_stop_requested=1;
    if(!__atomic_load_n(&foh_done,__ATOMIC_ACQUIRE)){
        uint64_t deadline;clock_interval_to_deadline(600,NSEC_PER_MSEC,&deadline);
        wait_result_t waiting=assert_wait_deadline((event_t)&foh_done,THREAD_UNINT,deadline);
        ml_set_interrupts_enabled(irq);
        if(waiting==THREAD_WAITING)thread_block(THREAD_CONTINUE_NULL);
    }else ml_set_interrupts_enabled(irq);
    if(!__atomic_load_n(&foh_done,__ATOMIC_ACQUIRE))foh_reason=10;
}
static void foh_stop(void)
{
    if(!foh_writes || foh_restored)return;
    /* Emergency best effort only. Never counts as an acknowledged restoration.
     * Private buffers stay allocated even after this write. */
    struct fire_display_io io={NULL,fso_read};struct fire_display_snapshot gate={0};
    if(fire_display_module_gate(&io,&gate,1u<<10)){foh_reason=11;return;}
    foh_emergency=1;
    fu_scanout_write(fop_bank,0xf40,FOH_ORIGINAL);
}
static void foh_report(void)
{
    char line[320];snprintf(line,sizeof(line),"fire-ovl-handoff: ready=%u phase=%u count=%u reason=%u writes=%u restored=%u emergency=%u retained=%llu pa0=%#x pa1=%#x\n",foh_ready,foh_phase,foh_count,foh_reason,foh_writes,foh_restored,foh_emergency,(unsigned long long)foh_retained,foh_pa[0],foh_pa[1]);loader_log_line(line);
#if defined(FIRE_OVL_REUSE_PROBE)
    snprintf(line,sizeof(line),"fire-ovl-reuse: rewritten=%u cleaned=%u at=%llu active=%#x retired=%#x before=0xff13579b after=0xff71c35e status=%#x flow=%#x\n",foh_rewritten,foh_rewrite_cleaned,(unsigned long long)foh_rewrite_at,foh_pa[1],foh_pa[0],foh_rewrite_status,foh_rewrite_flow);loader_log_line(line);
#endif
    for(unsigned i=0;i<foh_writes && i<4;i++){struct foh_write *s=&foh_write_log[i];
        snprintf(line,sizeof(line),"fire-ovl-handoff-write: index=%u at=%llu address=%#x status=%#x flow=%#x\n",i,(unsigned long long)s->at,s->address,s->status,s->flow);loader_log_line(line);}
    for(unsigned i=0;i<foh_count;i++){struct foh_sample *s=&foh_samples[i];
        snprintf(line,sizeof(line),"fire-ovl-handoff-sample: index=%u at=%llu request_at=%llu phase=%u address=%#x expected=%#x source=%#x debug=%#x position=%#x status=%#x flow=%#x match=%u\n",i,(unsigned long long)s->at,(unsigned long long)s->request_at,s->phase,s->address,s->expected,s->source,s->debug,s->position,s->status,s->flow,s->match);loader_log_line(line);}
}
