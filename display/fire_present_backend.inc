/* Karnak-only adapter for explicit mode2. Native UI validation is pending.
 * Native433/434 qualify the pool/address/cache route. This adapter adds real
 * acknowledgement readback and lifetime integration; native UI validation is
 * still required. The fixed pool stays allocated through kernel return. */
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <mach/vm_statistics.h>
#include "fire_present.h"
#include "fire_present_protocol.h"
#include "fire_present_trace.h"
#if defined(FIRE_BUFFERED_COVERED_SEED)
#include "fire_seed_coverage.h"
#if FPP_BYTES != FSC_FRAME_BYTES
#error "Covered seed requires the exact framebuffer byte contract"
#endif
__attribute__((used)) const char fire_present_seed_identity[]="fire-covered-seed-v1";
#endif
__attribute__((used)) const char fire_present_root_identity[]="fire-buffered-root:" FIRE_BUFFERED_ROOT_SHA;
__attribute__((used)) const char fire_present_request_time_identity[]="fire-present-request-program-time-v1";
extern uint64_t kvtophys(vm_offset_t);
extern unsigned fire_smp_ui_cpus(void);
extern int fire_ovl_gic_attach(uint32_t *,void(*)(void *,uint32_t),void *);
extern int fire_ovl_gic_restore(uint32_t *);
extern unsigned fire_ovl_gic_burst(void);
static lck_spin_t *fpr_lock;
static struct fpb fpr_owner;
static struct fpp fpr_protocol;
static vm_offset_t fpr_pixels[FPB_SLOTS],fpr_bank;
static uint32_t fpr_pa[FPB_SLOTS],fpr_saved[8],fpr_inten;
static uint64_t fpr_copy[FPB_SLOTS],fpr_draw_serial,fpr_draw_cookie,fpr_draw_tid;
static uint32_t fpr_swap[FPB_SLOTS];
static unsigned fpr_attempted,fpr_disabled,fpr_ready,fpr_hardware_ready,fpr_armed;
static unsigned fpr_attached,fpr_restored,fpr_stopped,fpr_emergency,fpr_reason,fpr_prepared,fpr_irq_enabled;
static unsigned fpr_pending_work;
static uint64_t fpr_irq_sequence,fpr_published,fpr_requested,fpr_completed,fpr_seed_bytes;
static uint64_t fpr_cache_bytes,fpr_waits,fpr_reads,fpr_retained,fpr_invalid_calls,fpr_extra_status,fpr_late_programs;
static boolean_t fpr_enter(void){boolean_t irq=ml_set_interrupts_enabled(FALSE);lck_spin_lock(fpr_lock);return irq;}
static void fpr_leave(boolean_t irq){lck_spin_unlock(fpr_lock);ml_set_interrupts_enabled(irq);}
static void fpr_wake(void){thread_wakeup((event_t)&fpr_owner);}
/* Lock-free observer read: never acquire the board lock under the profiler
 * writer gate. Renderer scopes and the actual GIC-body spans cover producer
 * admission and IRQ completion/trace emission around this queued-work bit. */
unsigned fire_present_profile_pending(void){return __atomic_load_n(&fpr_pending_work,__ATOMIC_ACQUIRE);}
static void fpr_sync_pending_locked(void)
{
    __atomic_store_n(&fpr_pending_work,fpr_owner.drawing!=FPB_NONE ||
        fpr_owner.ready!=FPB_NONE || fpr_owner.pending!=FPB_NONE ||
        fpr_protocol.pending.token!=0,__ATOMIC_RELEASE);
}
/* Capture before unlock: a completed/aborted slot can be reused immediately by
 * another CPU. The telemetry sink must never read live per-slot metadata. */
static struct fire_present_trace_event fpr_trace_snapshot(unsigned kind,uint64_t at,uint64_t end,uint64_t token,unsigned slot,unsigned status,unsigned flow)
{
    struct fire_present_trace_event e={kind,slot,status,flow,at,end,token,
        slot<FPB_SLOTS?fpr_copy[slot]:0,slot<FPB_SLOTS?fpr_swap[slot]:0,
        slot<FPB_SLOTS?fpr_pa[slot]:0,thread_tid(current_thread())};
    return e;
}
static unsigned fpr_power_gate(void)
{
    struct fire_display_io io={NULL,fso_read};struct fire_display_snapshot gate={0};
    return fire_display_module_gate(&io,&gate,1u<<10);
}
/* Caller holds ownership lock, but this guard has no callback into the lock. */
static int fpr_arm_locked(void)
{
    if(fpr_irq_enabled)return 1;
    if(fpr_power_gate()){fpr_reason=20;return 0;}
    if(!fpr_protocol.pending.token){uint32_t status=fu_r32(fpr_bank,8);if(status)fu_scanout_write(fpr_bank,8,~status);}
    fu_scanout_write(fpr_bank,4,2);fpr_irq_enabled=1;return 1;
}
static wait_result_t fpr_wait_locked(uint64_t deadline,boolean_t irq)
{
    fpr_waits++;
    wait_result_t waiting=assert_wait_deadline((event_t)&fpr_owner,THREAD_UNINT,deadline);
    fpr_leave(irq);
    return waiting==THREAD_WAITING?thread_block(THREAD_CONTINUE_NULL):waiting;
}
int fire_present_mode(void)
{
    if(__atomic_load_n(&fpr_disabled,__ATOMIC_ACQUIRE))return 0;
    return __atomic_load_n(&fpr_ready,__ATOMIC_ACQUIRE) &&
        __atomic_load_n(&fpr_hardware_ready,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&fpr_reason,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&fpr_stopped,__ATOMIC_ACQUIRE)?1:-1;
}
int fire_present_address_allowed(uint32_t address)
{
    if(!fpr_lock)return address==0x98800000u;
    boolean_t irq=fpr_enter();
    int allowed=!fpr_emergency && (fpr_protocol.configured?
        fpp_address_allowed(&fpr_protocol,address):address==0x98800000u);
    fpr_leave(irq);return allowed;
}
static int fpr_frame_valid(const struct fire_present_frame *frame)
{
    return frame && frame->slot<FPB_SLOTS && frame->slot==fpr_owner.drawing &&
        frame->serial && frame->serial==fpr_draw_cookie &&
        frame->pixels==(uint8_t *)fpr_pixels[frame->slot] && fpr_draw_tid==thread_tid(current_thread());
}
#if defined(FIRE_BUFFERED_COVERED_SEED)
int fire_present_begin_covered(struct fire_present_frame *frame,unsigned timeout_ms,unsigned covered_rows)
#else
int fire_present_begin(struct fire_present_frame *frame,unsigned timeout_ms)
#endif
{
    if(!frame)return -1;*frame=(struct fire_present_frame){0};
    int mode=fire_present_mode();if(mode<=0)return mode;
    if(!timeout_ms || timeout_ms>600 || !ml_get_interrupts_enabled())return -1;
    uint64_t deadline;clock_interval_to_deadline(timeout_ms,NSEC_PER_MSEC,&deadline);
    for(;;){
        boolean_t irq=fpr_enter();unsigned slot,seed;
        if(fpr_reason || fpr_owner.stopping){fpr_leave(irq);return -1;}
        if(fpb_begin(&fpr_owner,&slot,&seed)){
            fpr_sync_pending_locked();
            if(fpr_draw_serial==UINT64_MAX){fpb_abort_draw(&fpr_owner,slot);fpr_sync_pending_locked();fpr_reason=21;fpr_leave(irq);return -1;}
            uint64_t serial=++fpr_draw_serial;fpr_draw_cookie=serial;
            fpr_copy[slot]=0;fpr_swap[slot]=0;
            fpr_draw_tid=thread_tid(current_thread());fpr_prepared=0;
            vm_offset_t to=fpr_pixels[slot],from=fpr_pixels[seed];fpr_leave(irq);
            unsigned seed_offset=0,seed_bytes=FPP_BYTES;
#if defined(FIRE_BUFFERED_COVERED_SEED)
            seed_offset=fsc_seed_offset(covered_rows);seed_bytes=FPP_BYTES-seed_offset;
#endif
            uint64_t at=mach_absolute_time();
            if(!seed){volatile uint32_t *src=(volatile uint32_t *)(from+seed_offset);uint32_t *dst=(uint32_t *)(to+seed_offset);
                for(unsigned i=0;i<seed_bytes/4;i++)dst[i]=src[i];}
            else memcpy((void *)(to+seed_offset),(const void *)(from+seed_offset),seed_bytes);
            uint64_t end=mach_absolute_time();
            irq=fpr_enter();fpr_seed_bytes+=seed_bytes;
            if(!fpb_seed_done(&fpr_owner,slot)){fpr_reason=22;fpr_leave(irq);fpr_wake();return -1;}
            if(fpr_reason || fpr_owner.stopping){fpb_abort_draw(&fpr_owner,slot);fpr_sync_pending_locked();fpr_draw_cookie=0;fpr_leave(irq);fpr_wake();return -1;}
            *frame=(struct fire_present_frame){(uint8_t *)to,serial,slot,0};
            unsigned seed_status=0;
#if defined(FIRE_BUFFERED_COVERED_SEED)
            seed_status=seed_bytes;
#endif
            struct fire_present_trace_event trace=fpr_trace_snapshot(FPR_TRACE_SEED,at,end,serial,slot,seed_status,0);
            fpr_leave(irq);fire_present_trace(&trace);return 1;
        }
        if(mach_absolute_time()>=deadline){fpr_leave(irq);return -1;}
        if(!fpr_arm_locked()){fpr_leave(irq);fpr_wake();return -1;}
        (void)fpr_wait_locked(deadline,irq);
    }
}
#if defined(FIRE_BUFFERED_COVERED_SEED)
int fire_present_begin(struct fire_present_frame *frame,unsigned timeout_ms)
{
    return fire_present_begin_covered(frame,timeout_ms,0);
}
#endif
int fire_present_prepare(struct fire_present_frame *frame)
{
    if(fire_present_mode()!=1)return 0;
    boolean_t irq=fpr_enter();
    if(!fpr_frame_valid(frame) || fpr_owner.stopping || fpr_reason){fpr_invalid_calls++;fpr_leave(irq);return 0;}
    if(fpr_prepared){frame->prepared=1;fpr_leave(irq);return 1;}
    vm_offset_t pixels=fpr_pixels[frame->slot];fpr_leave(irq);
    uint64_t at=mach_absolute_time();CleanPoC_DcacheRegion(pixels,FPP_BYTES);uint64_t end=mach_absolute_time();
    irq=fpr_enter();fpr_cache_bytes+=FPP_BYTES;
    if(!fpr_frame_valid(frame) || fpr_owner.stopping || fpr_reason){fpr_leave(irq);return 0;}
    fpr_prepared=1;frame->prepared=1;
    struct fire_present_trace_event trace=fpr_trace_snapshot(FPR_TRACE_CACHE,at,end,frame->serial,frame->slot,0,0);
    fpr_leave(irq);fire_present_trace(&trace);return 1;
}
int fire_present_publish(struct fire_present_frame *frame,unsigned swap,uint64_t copy_token)
{
    if(!fpr_lock)return 0;boolean_t irq=fpr_enter();
    if(!fpr_frame_valid(frame) || !fpr_prepared || !frame->prepared || fpr_reason || fpr_owner.stopping){
        fpr_invalid_calls++;fpr_leave(irq);return 0;}
    /* Arm before surrendering DRAWING. On failure the caller still owns an
     * abortable destination. The IRQ cannot observe READY until we unlock. */
    if(!fpr_arm_locked()){fpr_leave(irq);return 0;}
    if(!fpb_publish(&fpr_owner,frame->slot)){fpr_invalid_calls++;fpr_leave(irq);return 0;}
    unsigned slot=frame->slot;uint64_t serial=frame->serial,at=mach_absolute_time();
    fpr_copy[slot]=copy_token;fpr_swap[slot]=swap;fpr_published++;fpr_draw_cookie=0;fpr_prepared=0;
    struct fire_present_trace_event trace=fpr_trace_snapshot(FPR_TRACE_PREPARED,at,at,serial,slot,0,0);
    fpr_leave(irq);fpr_wake();fire_present_trace(&trace);return 1;
}
void fire_present_abort(struct fire_present_frame *frame)
{
    if(!fpr_lock)return;boolean_t irq=fpr_enter();
    if(!fpr_frame_valid(frame) || !fpb_abort_draw(&fpr_owner,frame->slot)){fpr_invalid_calls++;fpr_leave(irq);return;}
    unsigned slot=frame->slot;uint64_t serial=frame->serial,at=mach_absolute_time();
    struct fire_present_trace_event trace=fpr_trace_snapshot(FPR_TRACE_ABORT,at,at,serial,slot,0,0);
    fpr_sync_pending_locked();
    fpr_draw_cookie=0;fpr_prepared=0;fpr_leave(irq);fpr_wake();fire_present_trace(&trace);
}
int fire_present_front_begin(struct fire_present_read *read)
{
    if(!read)return 0;*read=(struct fire_present_read){0};
    if(fire_present_mode()!=1 || !ml_get_interrupts_enabled() || !fl_display_check())return 0;
    uint64_t deadline;clock_interval_to_deadline(120,NSEC_PER_MSEC,&deadline);
    for(;;){
        boolean_t irq=fpr_enter();
        if(fpr_reason || fpr_owner.stopping){fpr_leave(irq);return 0;}
        if(fpr_owner.pending==FPB_NONE && fpr_owner.ready==FPB_NONE && fpr_owner.front==fpr_protocol.front){
            struct fpb_read_lease lease;
            if(fpb_read_begin(&fpr_owner,fpr_owner.front,&lease)){
                *read=(struct fire_present_read){(const uint8_t *)fpr_pixels[lease.slot],lease.serial,lease.content,lease.slot};
                fpr_reads++;fpr_leave(irq);return 1;
            }
        }
        if(mach_absolute_time()>=deadline){fpr_leave(irq);return 0;}
        if(!fpr_arm_locked()){fpr_leave(irq);return 0;}
        (void)fpr_wait_locked(deadline,irq);
    }
}
void fire_present_front_end(struct fire_present_read *read)
{
    if(!read || !fpr_lock)return;boolean_t irq=fpr_enter();
    struct fpb_read_lease lease={read->slot,read->serial,read->content};
    if(read->slot>=FPB_SLOTS || read->pixels!=(const uint8_t *)fpr_pixels[read->slot] || !fpb_read_end(&fpr_owner,&lease))fpr_invalid_calls++;
    fpr_leave(irq);fpr_wake();
}
static void fpr_irq(void *context,uint32_t id)
{
    (void)context;
    if(id!=216 || !__atomic_load_n(&fpr_armed,__ATOMIC_ACQUIRE)){gic_v3_spi_mask(216);return;}
    if(fire_ovl_gic_burst()>64 || !fl_display_check()){
        boolean_t irq=fpr_enter();fpr_reason=23;fpr_leave(irq);gic_v3_spi_mask(216);fpr_wake();return;
    }
    uint64_t at=mach_absolute_time();uint32_t status=fu_r32(fpr_bank,8);
    if(!__atomic_load_n(&fpr_hardware_ready,__ATOMIC_ACQUIRE)){if(status)fu_scanout_write(fpr_bank,8,~status);return;}
    struct fpp_event e={++fpr_irq_sequence,at,status,fu_r32(fpr_bank,0x240),fu_r32(fpr_bank,0xf40),0};
    struct fpp_completion completion={0};unsigned requested_slot=FPB_NONE;uint64_t requested_token=0;
    struct fire_present_trace_event consumed_trace={0},requested_trace={0},cleared_trace={0};
    boolean_t irq=fpr_enter();
    if(status&~3u)fpr_extra_status++;
    if(!fpp_open_event(&fpr_protocol,&e)){fpr_reason=24;goto acknowledge;}
    if(fpr_protocol.pending.token){
        unsigned old=fpr_owner.front;uint64_t old_token=fpr_owner.slot[old].token;
        if(fpp_complete(&fpr_protocol,fpr_protocol.pending.token,&completion)){
            if(!fpb_latch(&fpr_owner,completion.token) || !fpb_retire(&fpr_owner,old,old_token)){fpr_reason=25;goto acknowledge;}
            fpr_completed++;
            consumed_trace=fpr_trace_snapshot(FPR_TRACE_CONSUMED,at,at,completion.token,completion.new_slot,status,e.flow);
        }
    }
    if(!fpr_reason && !fpr_protocol.pending.token && fpr_owner.pending==FPB_NONE && fpp_idle(&e)){
        /* Recheck the live blank/idle state immediately before authorization.
         * No copies/cache work/waits occur in this critical section. */
        uint32_t flow=fu_r32(fpr_bank,0x240),address=fu_r32(fpr_bank,0xf40);
        if((flow&0x3ffu)==2u && (flow&0xf0000u)==0xf0000u && address==fpr_pa[fpr_owner.front]){
            uint64_t token=0;
            int queued=fpr_owner.stopping?fpb_restore(&fpr_owner,&token):fpb_queue(&fpr_owner,&token);
            if(queued){
                unsigned slot=fpr_owner.pending;
                if(!fpp_request_begin(&fpr_protocol,token,slot)){fpr_reason=26;goto acknowledge;}
                /* IRQ entry can precede another CPU publishing READY while
                 * this handler waits for ownership. Stamp the programming
                 * request here under the lock,after authorization. Keep the
                 * real EOF event time for protocol qualification unchanged. */
                uint64_t requested_at=mach_absolute_time();
                fu_scanout_write(fpr_bank,0xf40,fpr_pa[slot]);fpr_requested++;
                requested_slot=slot;requested_token=token;
                requested_trace=fpr_trace_snapshot(FPR_TRACE_REQUEST,requested_at,requested_at,requested_token,requested_slot,status,e.flow);
                uint32_t after=fu_r32(fpr_bank,0x240);
                if((after&0x3ffu)!=2u || (after&0xf0000u)!=0xf0000u)fpr_late_programs++;
            }
        }
    }
acknowledge:
    if(status)fu_scanout_write(fpr_bank,8,~status);
    if(fpr_protocol.open){
        uint32_t after=fu_r32(fpr_bank,8);
        if(fpr_protocol.pending.token && !fpr_protocol.pending.cleared){
            uint64_t cleared_at=mach_absolute_time();
            if(fpp_mark_cleared(&fpr_protocol,fpr_protocol.pending.token,cleared_at,after))
                cleared_trace=fpr_trace_snapshot(FPR_TRACE_CLEARED,cleared_at,cleared_at,
                    fpr_protocol.pending.token,fpr_protocol.pending.slot,after,0);
        }
        (void)fpp_close_event(&fpr_protocol);
    }
    /* A drawer already owns a destination and may publish just after this
     * EOF. Keep its IRQ observation live across that work instead of clearing
     * newly accumulated status in a disable/re-arm cycle at publication.
     * Truly idle displays still disable EOF interrupts after all work drains. */
    if(!fpr_protocol.pending.token && fpr_owner.ready==FPB_NONE &&
       fpr_owner.drawing==FPB_NONE && !fpr_owner.stopping){
        fu_scanout_write(fpr_bank,4,0);fpr_irq_enabled=0;
    }
    fpr_sync_pending_locked();
    fpr_leave(irq);fpr_wake();
    if(consumed_trace.kind)fire_present_trace(&consumed_trace);
    if(requested_trace.kind)fire_present_trace(&requested_trace);
    if(cleared_trace.kind)fire_present_trace(&cleared_trace);
}
static void fpr_start(void)
{
    if(fpr_attempted)return;fpr_attempted=1;
    unsigned enabled=1,mode=0;PE_parse_boot_argn("fbp",&enabled,sizeof(enabled));
    if(!enabled){__atomic_store_n(&fpr_disabled,1u,__ATOMIC_RELEASE);return;}
    PE_parse_boot_argn("fs",&mode,sizeof(mode));
    if(enabled!=1 || mode!=2 || fire_smp_ui_cpus()!=4 || cpu_number()!=0 || PAGE_SHIFT!=12 || !fl_display_check()){fpr_reason=1;return;}
    lck_grp_t *group=lck_grp_alloc_init("fire_present",LCK_GRP_ATTR_NULL);
    if(!group || !(fpr_lock=lck_spin_alloc_init(group,LCK_ATTR_NULL))){fpr_reason=2;return;}
    fpb_init(&fpr_owner);fpr_pa[0]=0x98800000u;
    fpr_pixels[0]=ml_io_map(fpr_pa[0],FPP_BYTES);fpr_bank=ml_io_map(FDP_OVL,4096);
    if(!fpr_pixels[0] || !fpr_bank){fpr_reason=3;return;}
    for(unsigned i=1;i<FPB_SLOTS;i++){
        if(kmem_alloc_contig(kernel_map,&fpr_pixels[i],FPP_BYTES,PAGE_MASK,(ppnum_t)(0xffffffffull>>PAGE_SHIFT),0,KMA_KOBJECT|KMA_LOMEM,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS){fpr_reason=4;return;}
        fpr_retained+=FPP_BYTES;uint64_t pa=kvtophys(fpr_pixels[i]);
        if(!pa || pa+FPP_BYTES>0x100000000ull || (pa&PAGE_MASK)){fpr_reason=5;return;}
        for(unsigned offset=0;offset<FPP_BYTES;offset+=PAGE_SIZE)if(kvtophys(fpr_pixels[i]+offset)!=pa+offset){fpr_reason=5;return;}
        if(kvtophys(fpr_pixels[i]+FPP_BYTES-1)!=pa+FPP_BYTES-1){fpr_reason=5;return;}
        fpr_pa[i]=(uint32_t)pa;
    }
    if(!fpp_init(&fpr_protocol,fpr_pa)){fpr_reason=6;return;}
    fpr_inten=fu_r32(fpr_bank,4);
    __atomic_store_n(&fpr_ready,1u,__ATOMIC_RELEASE);__atomic_store_n(&fpr_armed,1u,__ATOMIC_RELEASE);
    int code=fire_ovl_gic_attach(fpr_saved,fpr_irq,NULL);
    fpr_attached=code==KERN_SUCCESS || fpr_saved[0]==216;
    if(code!=KERN_SUCCESS){__atomic_store_n(&fpr_armed,0u,__ATOMIC_RELEASE);fpr_reason=7;return;}
    fu_scanout_write(fpr_bank,4,0);fpr_irq_enabled=0;
    __atomic_store_n(&fpr_hardware_ready,1u,__ATOMIC_RELEASE);
}
static void fpr_drain(void)
{
    if(!fpr_lock || fpr_disabled)return;
    if(!ml_get_interrupts_enabled()){fpr_reason=27;return;}
    uint64_t deadline;clock_interval_to_deadline(600,NSEC_PER_MSEC,&deadline);
    for(;;){
        boolean_t irq=fpr_enter();
        if(!fpr_owner.stopping){fpb_stop(&fpr_owner);fpr_sync_pending_locked();__atomic_store_n(&fpr_stopped,1u,__ATOMIC_RELEASE);}
        if(fpb_drained(&fpr_owner) && (!fpr_protocol.configured || (!fpr_protocol.pending.token && !fpr_protocol.front))){fpr_leave(irq);return;}
        if(fpr_reason || mach_absolute_time()>=deadline){if(!fpr_reason)fpr_reason=28;fpr_leave(irq);return;}
        if(!fpr_arm_locked()){fpr_leave(irq);return;}
        (void)fpr_wait_locked(deadline,irq);
    }
}
static void fpr_stop(void)
{
    if(!fpr_attached || fpr_restored)return;
    boolean_t irq=fpr_enter();gic_v3_spi_mask(216);__atomic_store_n(&fpr_armed,0u,__ATOMIC_RELEASE);
    if(!fpr_power_gate()){
        if(!fpb_drained(&fpr_owner) || fpr_protocol.pending.token || fpr_protocol.front){
            fpr_emergency=1;fu_scanout_write(fpr_bank,0xf40,0x98800000u);
        }
        fu_scanout_write(fpr_bank,4,fpr_inten);
        if(fu_r32(fpr_bank,4)!=fpr_inten)fpr_reason=29;
    }else fpr_reason=30;
    fpr_restored=fire_ovl_gic_restore(fpr_saved)!=0;fpr_leave(irq);
}
static unsigned fpr_stats(char *out,unsigned cap)
{
    if(!fpr_lock || cap<512)return 0;
    boolean_t irq=fpr_enter();
    unsigned ready=fpr_ready,reason=fpr_reason,front=fpr_owner.front,pending=fpr_owner.pending,stopping=fpr_owner.stopping;
    unsigned drained=fpb_drained(&fpr_owner),restored=fpr_restored,emergency=fpr_emergency;
    uint64_t published=fpr_published,requested=fpr_requested,completed=fpr_completed,seed=fpr_seed_bytes,cache=fpr_cache_bytes;
    uint64_t waits=fpr_waits,reads=fpr_reads,retained=fpr_retained,invalid=fpr_invalid_calls,extra=fpr_extra_status,late=fpr_late_programs;
    fpr_leave(irq);
    int n=snprintf(out,cap,"# hw_present version=1 ready=%u reason=%u front=%u pending=%u stopping=%u drained=%u restored=%u emergency=%u published=%llu requested=%llu completed=%llu seed=%llu cache=%llu waits=%llu reads=%llu retained=%llu invalid=%llu extra=%llu late=%llu\n",ready,reason,front,pending,stopping,drained,restored,emergency,(unsigned long long)published,(unsigned long long)requested,(unsigned long long)completed,(unsigned long long)seed,(unsigned long long)cache,(unsigned long long)waits,(unsigned long long)reads,(unsigned long long)retained,(unsigned long long)invalid,(unsigned long long)extra,(unsigned long long)late);
    return n>0 && (unsigned)n<cap?(unsigned)n:0;
}
static void fpr_report(void)
{
    char line[768];unsigned n=fpr_stats(line,sizeof(line));if(n)loader_log_line(line);
}
