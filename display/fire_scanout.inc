#include "fire_scanout.h"
#include "fire_measure.h"
#include "fire_vblank_clock.h"
extern int fire_display_gic_attach(uint32_t *,void(*)(void *,uint32_t),void *);
extern int fire_display_gic_restore(uint32_t *);
extern unsigned fire_display_gic_burst(void);
extern uint64_t fire_display_gic_status(void);
extern void gic_v3_spi_mask(uint32_t);
static unsigned fso_attempted,fso_attached,fso_restored,fso_gate;
static struct fsi_state fso;
static struct fvc_clock fso_clock;
static unsigned fso_clock_mode,fso_clock_stopped,fso_clock_status;
static uint64_t fso_memory_at,fso_memory_sequence;
static uint64_t fso_clock_notes[FVC_NOTES];
/* Error-only attribution;no hot-path logging or additional IRQ work. stage0
 * means outside replay,not necessarily startup. Times/code_or are diagnostics. */
struct fso_fault {uint64_t count,first,last,stage[4];uint32_t code_or;};
static struct fso_fault fso_faults[FVC_FAULTS];
void fire_display_vblank_fault(unsigned kind,uint32_t code)
{
    if(!fso_clock_mode || kind>=FVC_FAULTS)return;
    struct fso_fault *p=&fso_faults[kind];uint64_t at=mach_absolute_time();
    unsigned op=FSI_LOAD(&fso.active);if(op>3)op=0;
    __atomic_fetch_add(&fso_clock_notes[FVC_ERROR],1,__ATOMIC_RELAXED);
    __atomic_fetch_add(&p->count,1,__ATOMIC_RELAXED);
    __atomic_fetch_add(&p->stage[op],1,__ATOMIC_RELAXED);
    __atomic_fetch_or(&p->code_or,code,__ATOMIC_RELAXED);
    uint64_t old=__atomic_load_n(&p->first,__ATOMIC_RELAXED);
    while((!old || at<old) && !__atomic_compare_exchange_n(&p->first,&old,at,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
    old=__atomic_load_n(&p->last,__ATOMIC_RELAXED);
    while(at>old && !__atomic_compare_exchange_n(&p->last,&old,at,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED)){}
}
void fire_display_vblank_note(unsigned kind,uint64_t value)
{
    if(fso_clock_mode && kind<FVC_NOTES)__atomic_fetch_add(&fso_clock_notes[kind],value,__ATOMIC_RELAXED);
}
int fire_display_vblank_snapshot(struct fvc_view *out)
{
    if(__atomic_load_n(&fso_clock_stopped,__ATOMIC_ACQUIRE))return FVC_STOPPED;
    unsigned status=__atomic_load_n(&fso_clock_status,__ATOMIC_ACQUIRE);
    if(status!=FVC_READY)return status;
    if(!fso_clock_mode || FSI_LOAD(&fso.reason) || __atomic_load_n(&fso_clock.invalid,__ATOMIC_SEQ_CST))return FVC_FAILED;
    if(!FSI_LOAD(&fso.armed) || !fvc_read(&fso_clock,out))return FVC_WAIT;
    return FVC_READY;
}
static vm_offset_t fso_banks[5];
static const uint32_t fso_pas[5]={FDP_SPM,FDP_TOP,FDP_INFRA,FDP_MM,FDP_RDMA};
static uint32_t fso_saved[8],fso_inten;

static int fso_read(void *ctx,uint32_t pa,unsigned off,uint32_t *out)
{
    (void)ctx;if(!out || off>=4096 || (off&3))return 0;
    for(unsigned i=0;i<5;i++)if(pa==fso_pas[i] && fso_banks[i]){*out=fu_r32(fso_banks[i],off);return 1;}
    return 0;
}
#if defined(FIRE_PRESENTATION_LINK)
#include "fire_link_display.h"
#include "fire_link.h"
#endif
#if defined(FIRE_OVL_FRAME_PROBE)
#include "fire_ovl_frame_probe.inc"
#endif
#if defined(FIRE_BUFFERED_PRESENTATION)
#include "fire_present_backend.inc"
#endif
static void fso_irq(void *ctx,uint32_t id)
{
    (void)ctx;
    if(id!=218 || !FSI_LOAD(&fso.armed)){gic_v3_spi_mask(218);return;}
    if(fire_display_gic_burst()>64){FSI_STORE(&fso.reason,2);FSI_STORE(&fso.armed,0);gic_v3_spi_mask(218);return;}
    uint64_t at=mach_absolute_time();
    struct fire_display_io io={NULL,fso_read};struct fire_display_snapshot gate={0};
    if(fire_display_module_gate(&io,&gate,1u<<12)){
        FSI_STORE(&fso.reason,4);FSI_STORE(&fso.armed,0);gic_v3_spi_mask(218);return;
    }
    unsigned status=fu_r32(fso_banks[4],4);
    unsigned had_initial=FSI_LOAD(&fso.initial);
    if(fsi_event(&fso,status,at)){gic_v3_spi_mask(218);return;}
    if(had_initial && fire_measure_enabled()){
        uint64_t read_at=mach_absolute_time();
#if defined(FIRE_PRESENTATION_LINK)
        fire_link_irq(at,read_at,FSI_LOAD(&fso.calls),status,fl_display_check()?1:(fl_display_reason<<8));
#else
        fire_measure_event(FNM_IRQ,at,read_at,FSI_LOAD(&fso.calls),status,0,0,0);
#endif
        /* At most1Hz piggybacked on existing display IRQs. Sampled free RAM,
         * not an allocation high-water mark or per-process working set. */
        if((status&2) && at-fso_memory_at>=13000000){
            extern unsigned int vm_page_free_count;
            fso_memory_at=at;
            fire_measure_event(FNM_MEMORY,at,mach_absolute_time(),++fso_memory_sequence,
                (uint64_t)__atomic_load_n(&vm_page_free_count,__ATOMIC_RELAXED)*PAGE_SIZE,0,0,0);
        }
    }
    /* Exclude the inherited latched delivery exactly as the existing observer.
     * Publish throughout IRQ ownership,including before any replay stage. */
    if(fso_clock_mode && had_initial && (status&2))fvc_publish(&fso_clock,at);
    if(status)fu_scanout_write(fso_banks[4],4,~status); /* vendor complement,not W1C */
    uint64_t dt=mach_absolute_time()-at;FSI_ADD(&fso.handler_ticks,dt);
    if(dt>FSI_LOAD(&fso.handler_max))FSI_STORE(&fso.handler_max,dt);
}
static void fso_fail(unsigned gate)
{
    fso_gate=gate;__atomic_store_n(&fso_clock_status,FVC_FAILED,__ATOMIC_RELEASE);
}
static void fu_scanout_start(void)
{
    if(fu_test_mode!=1)return;
    unsigned mode=0,short_mode=0;
    bool have_long=PE_parse_boot_argn("fire_disp",&mode,sizeof(mode));
    bool have_short=PE_parse_boot_argn("fdi",&short_mode,sizeof(short_mode));
#if defined(FIRE_NATIVE_MEASURE)
    /* fnm includes the existing display-observer mode; two extra arguments do
     * not fit the fixed608-byte boot buffer. Explicit conflicting modes reject. */
    unsigned native_mode=0;PE_parse_boot_argn("fnm",&native_mode,sizeof(native_mode));
    if(native_mode==1 && !have_long && !have_short){have_short=true;short_mode=2;}
#endif
    if((have_long && mode!=2) || (have_short && short_mode!=2) || (!have_long && !have_short))return;
    fire_return_bind_boot(); /* exclusive routing setup/cleanup belongs to CPU0 */
    fso_attempted=1;
    unsigned clock_mode=0;PE_parse_boot_argn("fvs",&clock_mode,sizeof(clock_mode));
    fso_clock_mode=clock_mode==1;
    if(fu_scanout_frequency()!=13000000){fso_fail(1);return;}
    struct fire_display_io io={NULL,fu_display_read};struct fire_display_snapshot s;
    fire_display_collect(&io,&s);
    if(s.gate || s.seen!=127 || s.rdma_gate || s.ovl_gate){fso_fail(2);return;}
    const uint32_t route[10]={1,0,1,1,1,0,0,0,0,0};
    for(unsigned i=0;i<10;i++)if(s.route[i]!=route[i]){fso_fail(3);return;}
    if((s.rdma[2]&3)!=1 || (s.rdma[3]&0xfff)!=800 || (s.rdma[4]&0xfffff)!=1280 || s.rdma[0]!=63){fso_fail(3);return;}
    fso_inten=s.rdma[0];
    for(unsigned i=0;i<5;i++){fso_banks[i]=ml_io_map(fso_pas[i],4096);if(!fso_banks[i]){fso_fail(4);return;}}
#if defined(FIRE_PRESENTATION_LINK)
    fl_display_start();
#endif
    /* Publish all context before attach: generic attach unmasks immediately. */
    FSI_STORE(&fso.armed,1);
    int result=fire_display_gic_attach(fso_saved,fso_irq,NULL);
    char line[192];snprintf(line,sizeof(line),"fire-scanout: start code=%d pol_saved=%u pol_applied=%u route=%#x\n",result,fso_saved[6],fso_saved[7],fso_saved[5]);loader_log_line(line);
    if(result!=KERN_SUCCESS){FSI_STORE(&fso.armed,0);fso_attached=fso_saved[0]==218;fso_fail(5);return;}
    fso_attached=1;__atomic_store_n(&fso_clock_status,FVC_READY,__ATOMIC_RELEASE);
#if defined(FIRE_BUFFERED_PRESENTATION)
    fpr_start();
#endif
#if defined(FIRE_OVL_FRAME_PROBE)
    fop_start();
#endif
}
static void fu_scanout_begin(unsigned op,uint64_t at){fsi_begin(&fso,op,at);}
static void fu_scanout_end(unsigned op,uint64_t at){fsi_end(&fso,op,at);}
static void fu_scanout_stop(void)
{
    if(!fso_attached || fso_restored)return;
    /* Normal caller is bound to CPU0; return hook already masks IRQs. No
     * allocations/logging/waits here, including emergency return paths. */
    boolean_t irq=ml_set_interrupts_enabled(FALSE);
#if defined(FIRE_BUFFERED_PRESENTATION)
    fpr_stop();
#endif
#if defined(FIRE_OVL_FRAME_PROBE)
    fop_stop();
#endif
    __atomic_store_n(&fso_clock_stopped,1u,__ATOMIC_RELEASE);
    __atomic_store_n(&fso_clock_status,FVC_STOPPED,__ATOMIC_RELEASE);
    gic_v3_spi_mask(218);FSI_STORE(&fso.armed,0);FSI_STORE(&fso.active,0);
    fso_restored=fire_display_gic_restore(fso_saved)!=0;
    ml_set_interrupts_enabled(irq);
}
static void fu_scanout_shutdown_report(void)
{
#if defined(FIRE_BUFFERED_PRESENTATION)
    /* Thread context,IRQs enabled. The masked loader-return hook only stops. */
    fpr_drain();
#endif
#if defined(FIRE_OVL_HANDOFF_PROBE)
    foh_drain();
#endif
    fu_scanout_stop();if(!fso_attempted)return;
#if defined(FIRE_BUFFERED_PRESENTATION)
    fpr_report();
#endif
#if defined(FIRE_OVL_FRAME_PROBE)
    fop_report();
#endif
    char clock_line[384];snprintf(clock_line,sizeof(clock_line),"fire-vblank: mode=%u published=%llu invalid=%u stopped=%u sent=%llu polls=%llu stale=%llu wait=%llu missed=%llu errors=%llu\n",
        fso_clock_mode,__atomic_load_n(&fso_clock.sequence,__ATOMIC_SEQ_CST)/2,__atomic_load_n(&fso_clock.invalid,__ATOMIC_SEQ_CST),__atomic_load_n(&fso_clock_stopped,__ATOMIC_ACQUIRE),
        FSI_LOAD(&fso_clock_notes[FVC_SENT]),FSI_LOAD(&fso_clock_notes[FVC_POLL]),FSI_LOAD(&fso_clock_notes[FVC_STALE]),FSI_LOAD(&fso_clock_notes[FVC_START_WAIT]),FSI_LOAD(&fso_clock_notes[FVC_MISSED]),FSI_LOAD(&fso_clock_notes[FVC_ERROR]));loader_log_line(clock_line);
    for(unsigned i=0;i<FVC_FAULTS;i++){
        struct fso_fault *p=&fso_faults[i];char detail[320];
        snprintf(detail,sizeof(detail),"fire-vblank-fault: kind=%u count=%llu first=%llu last=%llu outside=%llu prep=%llu slow=%llu fast=%llu code_or=%u\n",
            i,FSI_LOAD(&p->count),FSI_LOAD(&p->first),FSI_LOAD(&p->last),FSI_LOAD(&p->stage[0]),FSI_LOAD(&p->stage[1]),FSI_LOAD(&p->stage[2]),FSI_LOAD(&p->stage[3]),FSI_LOAD(&p->code_or));loader_log_line(detail);
    }
    char line[256];snprintf(line,sizeof(line),"fire-scanout: stop gate=%u attached=%u restored=%u reason=%u calls=%llu inten_saved=%#x pol_saved=%u pol_applied=%u route_live=%#llx\n",fso_gate,fso_attached,fso_restored,FSI_LOAD(&fso.reason),FSI_LOAD(&fso.calls),fso_inten,fso_saved[6],fso_saved[7],fire_display_gic_status());loader_log_line(line);
}
static unsigned fu_scanout_stats(char *out,unsigned cap)
{
    if(!fso_attempted)return 0;unsigned used=0;
#if defined(FIRE_BUFFERED_PRESENTATION)
    used=fpr_stats(out,cap);
#endif
#if defined(FIRE_OVL_FRAME_PROBE)
    used=fop_live_stats(out,cap);
#endif
#define FSO_LINE(...) do {int n=snprintf(out+used,cap-used,__VA_ARGS__);if(n<0 || (unsigned)n>=cap-used)return used;used+=(unsigned)n;}while(0)
    FSO_LINE("# hw_scanout version=2 gate=%u attached=%u armed=%u restored=%u reason=%u calls=%llu initial=%u initial_status=%#llx spurious=%llu handler_ticks=%llu handler_max=%llu inten_saved=%#x hz=13000000 pol_saved=%u pol_applied=%u route_live=%#llx\n",fso_gate,fso_attached,FSI_LOAD(&fso.armed),fso_restored,FSI_LOAD(&fso.reason),FSI_LOAD(&fso.calls),FSI_LOAD(&fso.initial),FSI_LOAD(&fso.initial_status),FSI_LOAD(&fso.spurious),FSI_LOAD(&fso.handler_ticks),FSI_LOAD(&fso.handler_max),fso_inten,fso_saved[6],fso_saved[7],fire_display_gic_status());
    for(unsigned op=1;op<=3;op++){
        struct fsi_stage *p=&fso.stages[op-1];
        FSO_LINE("# hw_scanout_stage op=%u begin=%llu end=%llu closed=%u started=%llu completed=%llu late=%llu first=%llu last=%llu frames=%llu ends=%llu abnormal=%llu underflow=%llu target=%llu intervals=%llu sum=%llu min=%llu max=%llu",op,FSI_LOAD(&p->begin),FSI_LOAD(&p->end),FSI_LOAD(&p->closed),FSI_LOAD(&p->started),FSI_LOAD(&p->completed),FSI_LOAD(&p->late),FSI_LOAD(&p->first),FSI_LOAD(&p->last),FSI_LOAD(&p->frames),FSI_LOAD(&p->ends),FSI_LOAD(&p->abnormal),FSI_LOAD(&p->underflow),FSI_LOAD(&p->target),FSI_LOAD(&p->intervals),FSI_LOAD(&p->sum),FSI_LOAD(&p->min),FSI_LOAD(&p->max));
        for(unsigned i=0;i<8;i++)FSO_LINE(" h%u=%llu",i,FSI_LOAD(&p->hist[i]));FSO_LINE("\n");
    }
#undef FSO_LINE
    return used;
}
